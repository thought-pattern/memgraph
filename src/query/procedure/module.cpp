// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include "query/procedure/module.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

extern "C" {
#include <dlfcn.h>
#include <fcntl.h>
}

#include <fmt/format.h>
#include <unistd.h>

#include "license/license.hpp"
#ifdef MG_PYTHON_SUPPORT
#include "py/py.hpp"
#endif
#include "query/procedure/callable_alias_mapper.hpp"
#include "query/procedure/mg_procedure_helpers.hpp"
#include "query/procedure/mg_procedure_impl.hpp"
#ifdef MG_PYTHON_SUPPORT
#include "query/procedure/py_module.hpp"
#endif
#include "utils/case_insensitve_set.hpp"
#include "utils/file.hpp"
#include "utils/logging.hpp"
#include "utils/memory.hpp"
#include "utils/message.hpp"

#include <mutex>
#include <shared_mutex>

namespace memgraph::query {
auto ReservedBuiltInModuleNames() -> ::memgraph::utils::CaseInsensitiveSet const &;
}  // namespace memgraph::query

namespace memgraph::query::procedure {

#ifdef MG_PYTHON_SUPPORT
constexpr const char *func_code =
    "import ast\n\n"
    "no_removals = ['collections', 'abc', 'sys', 'torch', 'torch_geometric', 'igraph', 'dgl', 'numpy', 'mgp', "
    "'_mgp']\n"
    "modules = set()\n\n"
    "def visit_Import(node):\n"
    "  for name in node.names:\n"
    "    mod_name = name.name.split('.')[0]\n"
    "    if mod_name not in no_removals:\n"
    "      modules.add(mod_name)\n\n"
    "def visit_ImportFrom(node):\n"
    "  if node.module is not None and node.level == 0:\n"
    "    mod_name = node.module.split('.')[0]\n"
    "    if mod_name not in no_removals:\n"
    "      modules.add(mod_name)\n"
    "node_iter = ast.NodeVisitor()\n"
    "node_iter.visit_Import = visit_Import\n"
    "node_iter.visit_ImportFrom = visit_ImportFrom\n"
    "node_iter.visit(ast.parse(code))\n";

void ProcessFileDependencies(std::filesystem::path file_path_, const char *module_path, const char *func_code,
                             PyObject *sys_mod_ref);
#endif

ModuleRegistry gModuleRegistry;

Module::~Module() {}

// Every procedure registered here ignores its `mgp_graph *`. `GraphAccess::None` is declared per
// procedure rather than for the module because declaring it is also what opts one into running with no
// transaction, which is only worth doing for those callers actually issue that way.
class BuiltinModule final : public Module {
 public:
  BuiltinModule();
  ~BuiltinModule() override;
  BuiltinModule(const BuiltinModule &) = delete;
  BuiltinModule(BuiltinModule &&) = delete;
  BuiltinModule &operator=(const BuiltinModule &) = delete;
  BuiltinModule &operator=(BuiltinModule &&) = delete;

  bool Close() override;

  const std::map<std::string, mgp_proc, std::less<>> *Procedures() const override;

  const std::map<std::string, mgp_trans, std::less<>> *Transformations() const override;

  const std::map<std::string, mgp_func, std::less<>> *Functions() const override;

  void AddProcedure(std::string_view name, mgp_proc proc);

  void AddTransformation(std::string_view name, mgp_trans trans);

  std::optional<std::filesystem::path> Path() const override { return std::nullopt; }

 private:
  /// Registered procedures
  std::map<std::string, mgp_proc, std::less<>> procedures_;
  std::map<std::string, mgp_trans, std::less<>> transformations_;
  std::map<std::string, mgp_func, std::less<>> functions_;
};

BuiltinModule::BuiltinModule() {}

BuiltinModule::~BuiltinModule() {}

bool BuiltinModule::Close() { return true; }

const std::map<std::string, mgp_proc, std::less<>> *BuiltinModule::Procedures() const { return &procedures_; }

const std::map<std::string, mgp_trans, std::less<>> *BuiltinModule::Transformations() const {
  return &transformations_;
}

const std::map<std::string, mgp_func, std::less<>> *BuiltinModule::Functions() const { return &functions_; }

void BuiltinModule::AddProcedure(std::string_view name, mgp_proc proc) { procedures_.emplace(name, std::move(proc)); }

void BuiltinModule::AddTransformation(std::string_view name, mgp_trans trans) {
  transformations_.emplace(name, std::move(trans));
}

namespace {

void RegisterMgLoad(ModuleRegistry *module_registry, BuiltinModule *module) {
  // Loading relies on the fact that regular procedure invocation through
  // CallProcedureCursor::Pull takes ModuleRegistry::lock_ with READ access. To
  // load modules we have to upgrade our READ access to WRITE access,
  // therefore we release the READ lock and invoke the load function which
  // takes the WRITE lock. Obviously, some other thread may take a READ or WRITE
  // lock during our transition when we hold no such lock. In this case it is
  // fine, because our builtin module cannot be unloaded and we are ok with
  // using the new state of module_registry when we manage to acquire the lock
  // we desire. Note, deadlock between threads should not be possible, because a
  // single thread may only take either a READ or a WRITE lock, it's not
  // possible for a thread to hold both. If a thread tries to do that, it will
  // deadlock immediately (no other thread needs to do anything).
  auto load_all_cb = [module_registry](
                         mgp_list * /*args*/, mgp_graph * /*graph*/, mgp_result * /*result*/, mgp_memory * /*memory*/) {
    module_registry->UnloadAndLoadModulesFromDirectories();
  };
  mgp_proc load_all("load_all", load_all_cb, utils::NewDeleteResource());
  module->AddProcedure("load_all", std::move(load_all));
  auto load_cb = [module_registry](mgp_list *args, mgp_graph * /*graph*/, mgp_result *result, mgp_memory * /*memory*/) {
    MG_ASSERT(Call<size_t>(mgp_list_size, args) == 1U, "Should have been type checked already");
    auto *arg = Call<mgp_value *>(mgp_list_at, args, 0);
    MG_ASSERT(CallBool(mgp_value_is_string, arg), "Should have been type checked already");
    const char *arg_as_string{nullptr};
    const auto err = mgp_value_get_string(arg, &arg_as_string);
    bool succ = false;
    if (err != mgp_error::MGP_ERROR_NO_ERROR) {
      succ = false;
    } else {
      succ = module_registry->LoadOrReloadModuleFromName(arg_as_string);
    }
    if (!succ) {
      MG_ASSERT(mgp_result_set_error_msg(result, "Failed to (re)load the module.") == mgp_error::MGP_ERROR_NO_ERROR);
    }
  };
  mgp_proc load("load", load_cb, utils::NewDeleteResource());
  MG_ASSERT(mgp_proc_add_arg(&load, "module_name", Call<mgp_type *>(mgp_type_string)) == mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("load", std::move(load));
}

namespace {
[[nodiscard]] bool IsFileEditable(const std::optional<std::filesystem::path> &path) {
  return path && access(path->c_str(), W_OK) == 0;
}

std::string GetPathString(const std::optional<std::filesystem::path> &path) {
  if (!path) {
    return "builtin";
  }

  std::error_code ec;
  const auto canonical_path = std::filesystem::canonical(*path, ec);
  if (!ec) {
    return canonical_path.generic_string();
  }
  const auto weak_path = std::filesystem::weakly_canonical(*path, ec);
  return ec ? path->generic_string() : weak_path.generic_string();
}
}  // namespace

void RegisterMgProcedures(std::map<std::string, std::shared_ptr<Module>, std::less<>> const *all_modules,
                          utils::RWLock *lock, BuiltinModule *builtin_module) {
  auto procedures_cb = [all_modules, lock](
                           mgp_list * /*args*/, mgp_graph * /*graph*/, mgp_result *result, mgp_memory *memory) {
    // For details on how the invocation is done, take a look at the
    // CallProcedureCursor::Pull implementation.
    auto guard = std::unique_lock{*lock};
    for (const auto &[module_name, module] : *all_modules) {
      // Return the results in sorted order by module and by procedure.
      static_assert(
          std::is_same_v<decltype(module->Procedures()), const std::map<std::string, mgp_proc, std::less<>> *>,
          "Expected module procedures to be sorted by name");

      const auto path = module->Path();
      const auto path_string = GetPathString(path);
      const auto is_editable = IsFileEditable(path);

      for (const auto &[proc_name, proc] : *module->Procedures()) {
        mgp_result_record *record{nullptr};
        if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
          return;
        }

        const auto path_value = GetStringValueOrSetError(path_string.c_str(), memory, result);
        if (!path_value) {
          return;
        }

        MgpUniquePtr<mgp_value> is_editable_value{nullptr, mgp_value_destroy};
        if (!TryOrSetError([&] { return CreateMgpObject(is_editable_value, mgp_value_make_bool, is_editable, memory); },
                           result)) {
          return;
        }

        utils::pmr::string full_name(module_name, memory->impl);
        full_name.append(1, '.');
        full_name.append(proc_name);
        const auto name_value = GetStringValueOrSetError(full_name.c_str(), memory, result);
        if (!name_value) {
          return;
        }

        std::stringstream ss;
        ss << module_name << ".";
        PrintProcSignature(proc, &ss);
        const auto signature = ss.str();
        const auto signature_value = GetStringValueOrSetError(signature.c_str(), memory, result);
        if (!signature_value) {
          return;
        }

        MgpUniquePtr<mgp_value> is_write_value{nullptr, mgp_value_destroy};
        if (!TryOrSetError(
                [&, &proc = proc] {
                  return CreateMgpObject(is_write_value,
                                         mgp_value_make_bool,
                                         proc.info.graph_access == GraphAccess::Write ? 1 : 0,
                                         memory);
                },
                result)) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "name", name_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "signature", signature_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "is_write", is_write_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "path", path_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "is_editable", is_editable_value.get())) {
          return;
        }
      }
    }
  };
  mgp_proc procedures("procedures", procedures_cb, utils::NewDeleteResource(), {.graph_access = GraphAccess::None});
  MG_ASSERT(mgp_proc_add_result(&procedures, "name", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "signature", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "is_write", Call<mgp_type *>(mgp_type_bool)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "is_editable", Call<mgp_type *>(mgp_type_bool)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  builtin_module->AddProcedure("procedures", std::move(procedures));
}

void RegisterMgTransformations(std::map<std::string, std::shared_ptr<Module>, std::less<>> const *all_modules,
                               utils::RWLock *lock, BuiltinModule *builtin_module) {
  auto transformations_cb = [all_modules, lock](
                                mgp_list * /*unused*/, mgp_graph * /*unused*/, mgp_result *result, mgp_memory *memory) {
    auto guard = std::unique_lock{*lock};
    for (const auto &[module_name, module] : *all_modules) {
      // Return the results in sorted order by module and by transformation.
      static_assert(
          std::is_same_v<decltype(module->Transformations()), const std::map<std::string, mgp_trans, std::less<>> *>,
          "Expected module transformations to be sorted by name");

      const auto path = module->Path();
      const auto path_string = GetPathString(path);
      const auto is_editable = IsFileEditable(path);

      for (const auto &[trans_name, proc] : *module->Transformations()) {
        mgp_result_record *record{nullptr};
        if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
          return;
        }

        const auto path_value = GetStringValueOrSetError(path_string.c_str(), memory, result);
        if (!path_value) {
          return;
        }

        MgpUniquePtr<mgp_value> is_editable_value{nullptr, mgp_value_destroy};
        if (!TryOrSetError([&] { return CreateMgpObject(is_editable_value, mgp_value_make_bool, is_editable, memory); },
                           result)) {
          return;
        }

        utils::pmr::string full_name(module_name, memory->impl);
        full_name.append(1, '.');
        full_name.append(trans_name);

        const auto name_value = GetStringValueOrSetError(full_name.c_str(), memory, result);
        if (!name_value) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "name", name_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "path", path_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "is_editable", is_editable_value.get())) {
          return;
        }
      }
    }
  };
  mgp_proc procedures(
      "transformations", transformations_cb, utils::NewDeleteResource(), {.graph_access = GraphAccess::None});
  MG_ASSERT(mgp_proc_add_result(&procedures, "name", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&procedures, "is_editable", Call<mgp_type *>(mgp_type_bool)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  builtin_module->AddProcedure("transformations", std::move(procedures));
}

void RegisterMgFunctions(std::map<std::string, std::shared_ptr<Module>, std::less<>> const *all_modules,
                         utils::RWLock *lock, BuiltinModule *builtin_module) {
  auto functions_cb = [all_modules, lock](
                          mgp_list * /*args*/, mgp_graph * /*graph*/, mgp_result *result, mgp_memory *memory) {
    auto guard = std::unique_lock{*lock};
    for (const auto &[module_name, module] : *all_modules) {
      // Return the results in sorted order by module and by function_name.
      static_assert(std::is_same_v<decltype(module->Functions()), const std::map<std::string, mgp_func, std::less<>> *>,
                    "Expected module magic functions to be sorted by name");

      const auto path = module->Path();
      const auto path_string = GetPathString(path);
      const auto is_editable = IsFileEditable(path);

      for (const auto &[func_name, func] : *module->Functions()) {
        mgp_result_record *record{nullptr};

        if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
          return;
        }

        const auto path_value = GetStringValueOrSetError(path_string.c_str(), memory, result);
        if (!path_value) {
          return;
        }

        MgpUniquePtr<mgp_value> is_editable_value{nullptr, mgp_value_destroy};
        if (!TryOrSetError([&] { return CreateMgpObject(is_editable_value, mgp_value_make_bool, is_editable, memory); },
                           result)) {
          return;
        }

        utils::pmr::string full_name(module_name, memory->impl);
        full_name.append(1, '.');
        full_name.append(func_name);
        const auto name_value = GetStringValueOrSetError(full_name.c_str(), memory, result);
        if (!name_value) {
          return;
        }

        std::stringstream ss;
        ss << module_name << ".";
        PrintFuncSignature(func, ss);
        const auto signature = ss.str();
        const auto signature_value = GetStringValueOrSetError(signature.c_str(), memory, result);
        if (!signature_value) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "name", name_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "signature", signature_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "path", path_value.get())) {
          return;
        }

        if (!InsertResultOrSetError(result, record, "is_editable", is_editable_value.get())) {
          return;
        }
      }
    }
  };
  mgp_proc functions("functions", functions_cb, utils::NewDeleteResource(), {.graph_access = GraphAccess::None});
  MG_ASSERT(mgp_proc_add_result(&functions, "name", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&functions, "signature", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&functions, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&functions, "is_editable", Call<mgp_type *>(mgp_type_bool)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  builtin_module->AddProcedure("functions", std::move(functions));
}

namespace {
bool IsAllowedExtension(const auto &extension) {
  static constexpr std::array<std::string_view, 1> allowed_extensions{".py"};
  return std::ranges::any_of(allowed_extensions,
                             [&](const auto allowed_extension) { return allowed_extension == extension; });
}

bool IsLoadableModuleExtension(const auto &extension) {
  static constexpr std::array<std::string_view, 2> loadable_extensions{".py", ".so"};
  return std::ranges::any_of(loadable_extensions,
                             [&](const auto loadable_extension) { return loadable_extension == extension; });
}

bool IsSubPath(const auto &base, const auto &destination) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(destination, base, ec);
  if (ec) {
    return false;
  }
  return !relative.empty() && *relative.begin() != "..";
}

std::optional<std::string> ReadFile(const auto &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad() || (file.fail() && !file.eof())) {
    return std::nullopt;
  }
  return std::move(buffer).str();
}

// Return the module directory that contains the `path`
std::expected<std::filesystem::path, const char *> ParentModuleDirectory(const ModuleRegistry &module_registry,
                                                                         const std::filesystem::path &path) {
  const auto &module_directories = module_registry.GetModulesDirectory();

  auto longest_parent_directory = module_directories.end();
  auto max_length = std::numeric_limits<uint64_t>::min();
  for (auto it = module_directories.begin(); it != module_directories.end(); ++it) {
    if (IsSubPath(*it, path)) {
      std::error_code ec;
      const auto canon = std::filesystem::canonical(*it, ec);
      if (ec) {
        continue;
      }
      const auto length = canon.string().size();
      if (length > max_length) {
        longest_parent_directory = it;
        max_length = length;
      }
    }
  }

  if (longest_parent_directory == module_directories.end()) {
    return std::unexpected("The specified file isn't contained in any of the module directories.");
  }

  return *longest_parent_directory;
}
}  // namespace

void RegisterMgGetModuleFiles(ModuleRegistry *module_registry, BuiltinModule *module) {
  auto get_module_files_cb = [module_registry](
                                 mgp_list * /*args*/, mgp_graph * /*unused*/, mgp_result *result, mgp_memory *memory) {
    for (const auto &module_directory : module_registry->GetModulesDirectory()) {
      std::error_code ec;
      auto it = std::filesystem::recursive_directory_iterator(module_directory, ec);
      if (ec) {
        spdlog::warn("Unable to read module directory {}: {}", module_directory.string(), ec.message());
        continue;
      }
      for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
          spdlog::warn("Error while reading module directory {}: {}", module_directory.string(), ec.message());
          break;
        }
        const auto &dir_entry = *it;
        std::error_code entry_ec;
        if (dir_entry.is_regular_file(entry_ec) && !entry_ec &&
            IsLoadableModuleExtension(dir_entry.path().extension())) {
          mgp_result_record *record{nullptr};
          if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
            return;
          }

          const auto path_string = GetPathString(dir_entry.path());
          const auto is_editable =
              IsAllowedExtension(dir_entry.path().extension()) && IsFileEditable(dir_entry.path());

          const auto path_value = GetStringValueOrSetError(path_string.c_str(), memory, result);
          if (!path_value) {
            return;
          }

          MgpUniquePtr<mgp_value> is_editable_value{nullptr, mgp_value_destroy};
          if (!TryOrSetError(
                  [&] { return CreateMgpObject(is_editable_value, mgp_value_make_bool, is_editable, memory); },
                  result)) {
            return;
          }

          if (!InsertResultOrSetError(result, record, "path", path_value.get())) {
            return;
          }

          if (!InsertResultOrSetError(result, record, "is_editable", is_editable_value.get())) {
            return;
          }
        } else if (entry_ec) {
          spdlog::warn("Unable to inspect entry {} in module directory {}: {}",
                       dir_entry.path().string(),
                       module_directory.string(),
                       entry_ec.message());
        }
      }
    }
  };

  mgp_proc get_module_files(
      "get_module_files",
      get_module_files_cb,
      utils::NewDeleteResource(),
      {.graph_access = GraphAccess::None, .required_privilege = AuthQuery::Privilege::MODULE_READ});
  MG_ASSERT(mgp_proc_add_result(&get_module_files, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&get_module_files, "is_editable", Call<mgp_type *>(mgp_type_bool)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("get_module_files", std::move(get_module_files));
}

void RegisterMgGetModuleFile(ModuleRegistry *module_registry, BuiltinModule *module) {
  auto get_module_file_cb = [module_registry](
                                mgp_list *args, mgp_graph * /*unused*/, mgp_result *result, mgp_memory *memory) {
    MG_ASSERT(Call<size_t>(mgp_list_size, args) == 1U, "Should have been type checked already");
    auto *arg = Call<mgp_value *>(mgp_list_at, args, 0);
    MG_ASSERT(CallBool(mgp_value_is_string, arg), "Should have been type checked already");
    const char *path_str{nullptr};
    size_t path_len{0};
    if (!TryOrSetError([&] { return mgp_value_get_string(arg, &path_str); }, result) ||
        !TryOrSetError([&] { return mgp_value_get_string_size(arg, &path_len); }, result)) {
      return;
    }

    const std::filesystem::path path{std::string_view{path_str, path_len}};
    const auto maybe_content = module_registry->ReadModuleFileContent(path);
    if (!maybe_content) {
      static_cast<void>(mgp_result_set_error_msg(result, maybe_content.error().c_str()));
      return;
    }

    mgp_result_record *record{nullptr};
    if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
      return;
    }

    MgpUniquePtr<mgp_value> content_value{nullptr, mgp_value_destroy};
    if (!TryOrSetError(
            [&] {
              return CreateMgpObject(
                  content_value, mgp_value_make_string_with_size, maybe_content->data(), maybe_content->size(), memory);
            },
            result)) {
      return;
    }

    if (!InsertResultOrSetError(result, record, "content", content_value.get())) {
      return;
    }
  };
  mgp_proc get_module_file("get_module_file",
                           std::move(get_module_file_cb),
                           utils::NewDeleteResource(),
                           {.required_privilege = AuthQuery::Privilege::MODULE_READ});
  MG_ASSERT(mgp_proc_add_arg(&get_module_file, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&get_module_file, "content", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("get_module_file", std::move(get_module_file));
}

namespace {
std::expected<void, std::string> WriteAllToFd(int fd, std::string_view content, const std::filesystem::path &file) {
  size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(
          fmt::format("Failed to write the file at location {}: {}", file, std::strerror(errno)));
    }
    written += static_cast<size_t>(n);
  }
  return {};
}

std::expected<void, std::string> CreateFileExclusive(const std::filesystem::path &file, std::string_view content) {
  // NOLINTNEXTLINE(hicpp-signed-bitwise,cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return std::unexpected("File with the same name already exists!");
    }
    return std::unexpected(fmt::format("Failed to open the file at location {}: {}", file, std::strerror(errno)));
  }
  if (auto write_res = WriteAllToFd(fd, content, file); !write_res.has_value()) {
    static_cast<void>(::close(fd));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return std::unexpected(std::move(write_res.error()));
  }
  if (::close(fd) != 0) {
    const int err = errno;
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return std::unexpected(fmt::format("Failed to close the file at location {}: {}", file, std::strerror(err)));
  }
  return {};
}

std::expected<void, std::string> WriteToFileAtomic(const std::filesystem::path &file, std::string_view content) {
  static std::atomic<uint64_t> temp_counter{0};
  const auto temp_name =
      fmt::format(".{}.tmp.{}.{}", file.filename().string(), ::getpid(), temp_counter.fetch_add(1));
  const auto temp_path = file.parent_path() / temp_name;

  // NOLINTNEXTLINE(hicpp-signed-bitwise,cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    return std::unexpected(
        fmt::format("Failed to create staging file for {}: {}", file, std::strerror(errno)));
  }
  if (auto write_res = WriteAllToFd(fd, content, temp_path); !write_res.has_value()) {
    static_cast<void>(::close(fd));
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return std::unexpected(std::move(write_res.error()));
  }
  if (::close(fd) != 0) {
    const int err = errno;
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return std::unexpected(fmt::format("Failed to close staging file for {}: {}", file, std::strerror(err)));
  }
  std::error_code rename_ec;
  std::filesystem::rename(temp_path, file, rename_ec);
  if (rename_ec) {
    std::error_code rm_ec;
    std::filesystem::remove(temp_path, rm_ec);
    return std::unexpected(fmt::format("Failed to replace {}: {}", file, rename_ec.message()));
  }
  return {};
}
}  // namespace

void RegisterMgCreateModuleFile(ModuleRegistry *module_registry, BuiltinModule *module) {
  auto create_module_file_cb = [module_registry](
                                   mgp_list *args, mgp_graph * /*unused*/, mgp_result *result, mgp_memory *memory) {
    MG_ASSERT(Call<size_t>(mgp_list_size, args) == 2U, "Should have been type checked already");
    auto *filename_arg = Call<mgp_value *>(mgp_list_at, args, 0);
    MG_ASSERT(CallBool(mgp_value_is_string, filename_arg), "Should have been type checked already");
    const char *filename_str{nullptr};
    size_t filename_len{0};
    if (!TryOrSetError([&] { return mgp_value_get_string(filename_arg, &filename_str); }, result) ||
        !TryOrSetError([&] { return mgp_value_get_string_size(filename_arg, &filename_len); }, result)) {
      return;
    }

    auto *content_arg = Call<mgp_value *>(mgp_list_at, args, 1);
    MG_ASSERT(CallBool(mgp_value_is_string, content_arg), "Should have been type checked already");
    const char *content_str{nullptr};
    size_t content_len{0};
    if (!TryOrSetError([&] { return mgp_value_get_string(content_arg, &content_str); }, result) ||
        !TryOrSetError([&] { return mgp_value_get_string_size(content_arg, &content_len); }, result)) {
      return;
    }

    const auto maybe_created = module_registry->CreateModuleFile(
        std::string_view{filename_str, filename_len}, std::string_view{content_str, content_len});
    if (!maybe_created.has_value()) {
      static_cast<void>(mgp_result_set_error_msg(result, maybe_created.error().c_str()));
      return;
    }

    mgp_result_record *record{nullptr};
    if (!TryOrSetError([&] { return mgp_result_new_record(result, &record); }, result)) {
      return;
    }

    const auto path_str = GetPathString(*maybe_created);
    const auto path_value = GetStringValueOrSetError(path_str.c_str(), memory, result);
    if (!path_value) {
      return;
    }

    if (!InsertResultOrSetError(result, record, "path", path_value.get())) {
      return;
    }
  };
  mgp_proc create_module_file("create_module_file",
                              std::move(create_module_file_cb),
                              utils::NewDeleteResource(),
                              {.required_privilege = AuthQuery::Privilege::MODULE_WRITE});
  MG_ASSERT(mgp_proc_add_arg(&create_module_file, "filename", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_arg(&create_module_file, "content", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_result(&create_module_file, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("create_module_file", std::move(create_module_file));
}

void RegisterMgUpdateModuleFile(ModuleRegistry *module_registry, BuiltinModule *module) {
  auto update_module_file_cb =
      [module_registry](mgp_list *args, mgp_graph * /*unused*/, mgp_result *result, mgp_memory * /*memory*/) {
        MG_ASSERT(Call<size_t>(mgp_list_size, args) == 2U, "Should have been type checked already");
        auto *path_arg = Call<mgp_value *>(mgp_list_at, args, 0);
        MG_ASSERT(CallBool(mgp_value_is_string, path_arg), "Should have been type checked already");
        const char *path_str{nullptr};
        size_t path_len{0};
        if (!TryOrSetError([&] { return mgp_value_get_string(path_arg, &path_str); }, result) ||
            !TryOrSetError([&] { return mgp_value_get_string_size(path_arg, &path_len); }, result)) {
          return;
        }

        auto *content_arg = Call<mgp_value *>(mgp_list_at, args, 1);
        MG_ASSERT(CallBool(mgp_value_is_string, content_arg), "Should have been type checked already");
        const char *content_str{nullptr};
        size_t content_len{0};
        if (!TryOrSetError([&] { return mgp_value_get_string(content_arg, &content_str); }, result) ||
            !TryOrSetError([&] { return mgp_value_get_string_size(content_arg, &content_len); }, result)) {
          return;
        }

        const std::filesystem::path path{std::string_view{path_str, path_len}};
        if (auto maybe_error =
                module_registry->UpdateModuleFile(path, std::string_view{content_str, content_len});
            !maybe_error.has_value()) {
          static_cast<void>(mgp_result_set_error_msg(result, maybe_error.error().c_str()));
          return;
        }
      };
  mgp_proc update_module_file("update_module_file",
                              std::move(update_module_file_cb),
                              utils::NewDeleteResource(),
                              {.required_privilege = AuthQuery::Privilege::MODULE_WRITE});
  MG_ASSERT(mgp_proc_add_arg(&update_module_file, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  MG_ASSERT(mgp_proc_add_arg(&update_module_file, "content", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("update_module_file", std::move(update_module_file));
}

void RegisterMgDeleteModuleFile(ModuleRegistry *module_registry, BuiltinModule *module) {
  auto delete_module_file_cb =
      [module_registry](mgp_list *args, mgp_graph * /*unused*/, mgp_result *result, mgp_memory * /*memory*/) {
        MG_ASSERT(Call<size_t>(mgp_list_size, args) == 1U, "Should have been type checked already");
        auto *path_arg = Call<mgp_value *>(mgp_list_at, args, 0);
        MG_ASSERT(CallBool(mgp_value_is_string, path_arg), "Should have been type checked already");
        const char *path_str{nullptr};
        size_t path_len{0};
        if (!TryOrSetError([&] { return mgp_value_get_string(path_arg, &path_str); }, result) ||
            !TryOrSetError([&] { return mgp_value_get_string_size(path_arg, &path_len); }, result)) {
          return;
        }

        const std::filesystem::path path{std::string_view{path_str, path_len}};
        if (auto maybe_error = module_registry->DeleteModuleFile(path); !maybe_error.has_value()) {
          static_cast<void>(mgp_result_set_error_msg(result, maybe_error.error().c_str()));
          return;
        }
      };
  mgp_proc delete_module_file("delete_module_file",
                              std::move(delete_module_file_cb),
                              utils::NewDeleteResource(),
                              {.required_privilege = AuthQuery::Privilege::MODULE_WRITE});
  MG_ASSERT(mgp_proc_add_arg(&delete_module_file, "path", Call<mgp_type *>(mgp_type_string)) ==
            mgp_error::MGP_ERROR_NO_ERROR);
  module->AddProcedure("delete_module_file", std::move(delete_module_file));
}

// Run `fun` with `mgp_module *` and `mgp_memory *` arguments. If `fun` returned
// a `true` value, store the `mgp_module::procedures` and
// `mgp_module::transformations into `proc_map`. The return value of WithModuleRegistration
// is the same as that of `fun`. Note, the return value need only be convertible to `bool`,
// it does not have to be `bool` itself.
template <class TProcMap, class TTransMap, class TFuncMap, class TFun>
auto WithModuleRegistration(TProcMap *proc_map, TTransMap *trans_map, TFuncMap *func_map, const TFun &fun) {
  // We probably don't need more than 256KB for module initialization.
  static constexpr size_t stack_bytes = 256UL * 1024UL;
  unsigned char stack_memory[stack_bytes];
  utils::MonotonicBufferResource monotonic_memory(stack_memory, stack_bytes);
  mgp_memory memory{&monotonic_memory};
  mgp_module module_def{memory.impl};
  auto res = fun(&module_def, &memory);
  if (res) {
    // Copy procedures into resulting proc_map.
    for (const auto &proc : module_def.procedures) proc_map->emplace(proc);
    // Copy transformations into resulting trans_map.
    for (const auto &trans : module_def.transformations) trans_map->emplace(trans);
    // Copy functions into resulting func_map.
    for (const auto &func : module_def.functions) func_map->emplace(func);
  }
  return res;
}

}  // namespace

class SharedLibraryModule final : public Module {
 public:
  SharedLibraryModule();
  ~SharedLibraryModule() override;
  SharedLibraryModule(const SharedLibraryModule &) = delete;
  SharedLibraryModule(SharedLibraryModule &&) = delete;
  SharedLibraryModule &operator=(const SharedLibraryModule &) = delete;
  SharedLibraryModule &operator=(SharedLibraryModule &&) = delete;

  bool Load(const std::filesystem::path &file_path);

  bool Close() override;

  const std::map<std::string, mgp_proc, std::less<>> *Procedures() const override;

  const std::map<std::string, mgp_trans, std::less<>> *Transformations() const override;

  const std::map<std::string, mgp_func, std::less<>> *Functions() const override;

  std::optional<std::filesystem::path> Path() const override { return file_path_; }

 private:
  /// Path as requested for loading the module from a library.
  std::filesystem::path file_path_;
  /// Private staged path opened by dlopen so hot-reloads never collide with a still-open handle at file_path_.
  std::filesystem::path loaded_so_path_;
  /// System handle to shared library.
  void *handle_;
  /// Required initialization function called on module load.
  std::function<int(mgp_module *, mgp_memory *)> init_fn_;
  /// Optional shutdown function called on module unload.
  std::function<int()> shutdown_fn_;
  /// Registered procedures
  std::map<std::string, mgp_proc, std::less<>> procedures_;
  /// Registered transformations
  std::map<std::string, mgp_trans, std::less<>> transformations_;
  /// Registered functions
  std::map<std::string, mgp_func, std::less<>> functions_;
};

SharedLibraryModule::SharedLibraryModule() : handle_(nullptr) {}

SharedLibraryModule::~SharedLibraryModule() {
  if (handle_) Close();
}

bool SharedLibraryModule::Load(const std::filesystem::path &file_path) {
  MG_ASSERT(!handle_, "Attempting to load an already loaded module...");
  spdlog::info("Loading module {}...", file_path);
  file_path_ = file_path;
  auto cleanup_staged_so = [this] {
    if (!loaded_so_path_.empty()) {
      std::error_code rm_ec;
      std::filesystem::remove(loaded_so_path_, rm_ec);
      loaded_so_path_.clear();
    }
  };
  static std::atomic<uint64_t> next_so_stage_id{1};
  std::error_code stage_ec;
  const auto stage_dir = std::filesystem::temp_directory_path(stage_ec);
  if (!stage_ec) {
    const auto cand = stage_dir / fmt::format("mg_shared_mod_{}_{}{}",
                                              static_cast<uint64_t>(::getpid()),
                                              next_so_stage_id.fetch_add(1, std::memory_order_relaxed),
                                              file_path.extension().string());
    if (std::filesystem::copy_file(file_path, cand, std::filesystem::copy_options::overwrite_existing, stage_ec) &&
        !stage_ec) {
      loaded_so_path_ = cand;
    }
  }
  const auto &dlopen_target = loaded_so_path_.empty() ? file_path : loaded_so_path_;
  dlerror();  // Clear any existing error.
  // NOLINTNEXTLINE(hicpp-signed-bitwise)
  handle_ = dlopen(dlopen_target.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    spdlog::error(
        utils::MessageWithLink("Unable to load module {}; {}.", file_path, dlerror(), "https://memgr.ph/modules"));
    cleanup_staged_so();
    return false;
  }
  // Get required mgp_init_module
  init_fn_ = reinterpret_cast<int (*)(mgp_module *, mgp_memory *)>(dlsym(handle_, "mgp_init_module"));
  char *dl_errored = dlerror();
  if (!init_fn_ || dl_errored) {
    spdlog::error(
        utils::MessageWithLink("Unable to load module {}; {}.", file_path, dl_errored, "https://memgr.ph/modules"));
    dlclose(handle_);
    handle_ = nullptr;
    cleanup_staged_so();
    return false;
  }
  auto module_cb = [&](auto *module_def, auto *memory) {
    // Run mgp_init_module which must succeed.
    int init_res = init_fn_(module_def, memory);
    auto with_error = [this, &cleanup_staged_so](std::string_view error_msg) {
      spdlog::error(error_msg);
      dlclose(handle_);
      handle_ = nullptr;
      cleanup_staged_so();
      return false;
    };

    if (init_res != 0) {
      const auto error = fmt::format("Unable to load module {}; mgp_init_module_returned {} ", file_path, init_res);
      return with_error(error);
    }
    for (auto &trans : module_def->transformations) {
      const bool success = mgp_error::MGP_ERROR_NO_ERROR == MgpTransAddFixedResult(&trans.second);
      if (!success) {
        const auto error =
            fmt::format("Unable to add result to transformation in module {}; add result failed", file_path);
        return with_error(error);
      }
    }
    for (const auto &[proc_name, proc] : module_def->procedures) {
      if (proc.info.is_batched && proc.results.empty()) {
        return with_error(fmt::format(
            "Unable to load module {}; void procedures cannot be batched (procedure '{}' declares no result fields).",
            file_path,
            proc_name));
      }
    }
    return true;
  };
  if (!WithModuleRegistration(&procedures_, &transformations_, &functions_, module_cb)) {
    cleanup_staged_so();
    return false;
  }
  // Get optional mgp_shutdown_module
  shutdown_fn_ = reinterpret_cast<int (*)()>(dlsym(handle_, "mgp_shutdown_module"));
  dl_errored = dlerror();
  if (dl_errored) spdlog::warn("When loading module {}; {}", file_path, dl_errored);
  spdlog::info("Loaded module {}", file_path);
  return true;
}

bool SharedLibraryModule::Close() {
  MG_ASSERT(handle_, "Attempting to close a module that has not been loaded...");
  spdlog::info("Closing module {}...", file_path_);
  // non-existent shutdown function is semantically the same as a shutdown
  // function that does nothing.
  int shutdown_res = 0;
  if (shutdown_fn_) shutdown_res = shutdown_fn_();
  if (shutdown_res != 0) {
    spdlog::warn("When closing module {}; mgp_shutdown_module returned {}", file_path_, shutdown_res);
  }
  if (dlclose(handle_) != 0) {
    spdlog::error(
        utils::MessageWithLink("Failed to close module {}; {}.", file_path_, dlerror(), "https://memgr.ph/modules"));
    return false;
  }
  if (!loaded_so_path_.empty()) {
    std::error_code rm_ec;
    std::filesystem::remove(loaded_so_path_, rm_ec);
    loaded_so_path_.clear();
  }
  spdlog::info("Closed module {}", file_path_);
  handle_ = nullptr;
  procedures_.clear();
  return true;
}

const std::map<std::string, mgp_proc, std::less<>> *SharedLibraryModule::Procedures() const {
  MG_ASSERT(handle_,
            "Attempting to access procedures of a module that has not "
            "been loaded...");
  return &procedures_;
}

const std::map<std::string, mgp_trans, std::less<>> *SharedLibraryModule::Transformations() const {
  MG_ASSERT(handle_,
            "Attempting to access procedures of a module that has not "
            "been loaded...");
  return &transformations_;
}

const std::map<std::string, mgp_func, std::less<>> *SharedLibraryModule::Functions() const {
  MG_ASSERT(handle_,
            "Attempting to access functions of a module that has not "
            "been loaded...");
  return &functions_;
}

#ifdef MG_PYTHON_SUPPORT
class PythonModule final : public Module {
 public:
  PythonModule();
  ~PythonModule() override;
  PythonModule(const PythonModule &) = delete;
  PythonModule(PythonModule &&) = delete;
  PythonModule &operator=(const PythonModule &) = delete;
  PythonModule &operator=(PythonModule &&) = delete;

  bool Load(const std::filesystem::path &file_path);

  bool Close() override;

  const std::map<std::string, mgp_proc, std::less<>> *Procedures() const override;
  const std::map<std::string, mgp_trans, std::less<>> *Transformations() const override;
  const std::map<std::string, mgp_func, std::less<>> *Functions() const override;

  std::optional<std::filesystem::path> Path() const override { return file_path_; }

 private:
  std::filesystem::path file_path_;
  py::Object py_module_;
  std::map<std::string, mgp_proc, std::less<>> procedures_;
  std::map<std::string, mgp_trans, std::less<>> transformations_;
  std::map<std::string, mgp_func, std::less<>> functions_;
};

PythonModule::PythonModule() {}

PythonModule::~PythonModule() {
  try {
    if (py_module_) Close();
  } catch (std::exception const &e) {
    spdlog::error("Exception during PythonModule cleanup for {}, {}", file_path_.string(), e.what());
  }
}

bool PythonModule::Load(const std::filesystem::path &file_path) {
  MG_ASSERT(!py_module_, "Attempting to load an already loaded module...");
  spdlog::info("Loading module {}...", file_path);
  file_path_ = file_path;
  auto gil = py::EnsureGIL();
  auto maybe_exc = py::AppendToSysPath(file_path.parent_path().c_str());
  if (maybe_exc) {
    spdlog::error(
        utils::MessageWithLink("Unable to load module {}; {}.", file_path, *maybe_exc, "https://memgr.ph/modules"));
    return false;
  }
  bool succ = true;
  std::string batched_void_proc_name;
  auto module_cb = [&](auto *module_def, auto * /*memory*/) {
    auto result = ImportPyModuleFromPath(file_path, module_def);
    if (!result) {
      return result;
    }
    for (auto &trans : module_def->transformations) {
      succ = MgpTransAddFixedResult(&trans.second) == mgp_error::MGP_ERROR_NO_ERROR;
      if (!succ) {
        return result;
      }
    };
    for (const auto &[proc_name, proc] : module_def->procedures) {
      if (proc.info.is_batched && proc.results.empty()) {
        succ = false;
        batched_void_proc_name = proc_name;
        return result;
      }
    }
    return result;
  };
  py_module_ = WithModuleRegistration(&procedures_, &transformations_, &functions_, module_cb);
  if (py_module_) {
    if (!succ) {
      if (!batched_void_proc_name.empty()) {
        spdlog::error(
            "Unable to load module {}; void procedures cannot be batched (procedure '{}' declares no result fields).",
            file_path,
            batched_void_proc_name);
      } else {
        spdlog::error("Unable to add result to transformation");
      }
      Close();
      return false;
    }
    spdlog::info("Loaded module {}", file_path);
    return true;
  }
  auto exc_info = py::FetchError();
  spdlog::error(utils::MessageWithLink(
      "Unable to load module {}; {}.",
      file_path,
      exc_info ? py::FormatException(*exc_info) : std::string{"import failed"},
      "https://memgr.ph/modules"));
  return false;
}

bool PythonModule::Close() {
  MG_ASSERT(py_module_, "Attempting to close a module that has not been loaded...");
  spdlog::info("Closing module {}...", file_path_);

  // If the Python interpreter is finalizing, we should avoid complex cleanup
  // that involves running scripts, as it might fail or cause crashes.
  // We must check this *before* acquiring the GIL, as EnsureGIL will terminate
  // the thread if called during finalization.
  if (MG_PY_IS_FINALIZING()) {
    procedures_.clear();
    transformations_.clear();
    functions_.clear();
    py_module_ = py::Object(nullptr);
    return true;
  }

  auto gil = py::EnsureGIL();

  procedures_.clear();
  transformations_.clear();
  functions_.clear();

  // Get the reference to sys.modules dictionary. PyImport_GetModuleDict()
  // returns a borrowed reference to the internal modules dictionary.
  PyObject *sys_mod_ref = PyImport_GetModuleDict();
  if (!sys_mod_ref) {
    spdlog::warn("Failed to get sys.modules dictionary");
    py_module_ = py::Object(nullptr);
    return false;
  }

  std::string stem = file_path_.stem().string();
  PyObject *current_sys_mod = PyDict_GetItemString(sys_mod_ref, stem.c_str());
  const bool replaced_in_place = (current_sys_mod != nullptr && current_sys_mod != py_module_.Ptr());

  if (!replaced_in_place) {
    ProcessFileDependencies(file_path_, stem.c_str(), func_code, sys_mod_ref);

    std::vector<std::filesystem::path> submodules;

    for (auto it = std::filesystem::recursive_directory_iterator(file_path_.parent_path());
         it != std::filesystem::recursive_directory_iterator();
         ++it) {
      std::string dir_entry_stem = it->path().stem().string();
      if (it->is_regular_file() || dir_entry_stem == "__pycache__") continue;
      if (dir_entry_stem.contains(stem)) {
        it.disable_recursion_pending();
        submodules.emplace_back(it->path());
      }
    }

    for (const auto &submodule : submodules) {
      if (!std::filesystem::exists(submodule)) {
        continue;
      }
      std::error_code ec;
      std::filesystem::remove_all(submodule / "__pycache__", ec);
      if (ec) {
        spdlog::warn("Failed to remove __pycache__ in {}: {}", submodule.string(), ec.message());
      }
      auto rec_it = std::filesystem::recursive_directory_iterator(submodule, ec);
      if (ec) {
        spdlog::warn("Failed to iterate submodule {}: {}", submodule.string(), ec.message());
        continue;
      }
      for (; rec_it != std::filesystem::recursive_directory_iterator(); rec_it.increment(ec)) {
        if (ec) {
          spdlog::warn("Failed to iterate in {}: {}", submodule.string(), ec.message());
          break;
        }
        auto const &rec_dir_entry = *rec_it;
        std::string const rec_dir_entry_stem = rec_dir_entry.path().stem().string();
        if (rec_dir_entry.is_directory() && rec_dir_entry_stem != "__pycache__") {
          ec.clear();
          std::filesystem::remove_all(rec_dir_entry.path() / "__pycache__", ec);
          if (ec) {
            spdlog::warn("Failed to remove __pycache__ in {}: {}", rec_dir_entry.path().string(), ec.message());
          }
        }
        std::string const rec_dir_entry_ext = rec_dir_entry.path().extension().string();
        if (!rec_dir_entry.is_regular_file() || rec_dir_entry_ext != ".py") continue;
        ProcessFileDependencies(rec_dir_entry.path().c_str(), stem.c_str(), func_code, sys_mod_ref);
      }
    }

    if (current_sys_mod == py_module_.Ptr()) {
      if (PyDict_DelItemString(sys_mod_ref, stem.c_str()) != 0) {
        spdlog::warn("Failed to remove the module {} from sys.modules", stem.c_str());
        PyErr_Clear();
        py_module_ = py::Object(nullptr);
        return false;
      }
    }

    // Remove the cached bytecode if it's present
    std::filesystem::remove_all(file_path_.parent_path() / "__pycache__");
  }
  py_module_ = py::Object(nullptr);
  spdlog::info("Closed module {}", file_path_);
  return true;
}

// Must be called with GIL taken because of Py_DECREF
void ProcessFileDependencies(std::filesystem::path file_path_, const char *module_path, const char *func_code,
                             PyObject *sys_mod_ref) {
  MG_ASSERT(PyGILState_Check(), "ProcessFileDependencies requires the GIL to be held");
  std::optional<std::string> maybe_content;
  {
    Py_BEGIN_ALLOW_THREADS maybe_content = ReadFile(file_path_);
    Py_END_ALLOW_THREADS
  }

  if (maybe_content && !maybe_content->empty()) {
    const char *content_value = maybe_content->c_str();

    const py::Object py_main(PyImport_ImportModule("__main__"));
    if (!py_main) {
      PyErr_Clear();
      return;
    }

    PyObject *py_global_dict = PyModule_GetDict(py_main.Ptr());
    if (!py_global_dict) {
      PyErr_Clear();
      return;
    }

    const py::Object py_code_content(PyUnicode_FromString(content_value));
    if (!py_code_content) {
      PyErr_Clear();
      return;
    }

    // Run the dependency scanner in a sandbox dict copied from __main__ so that
    // the scanner script cannot corrupt __main__'s global namespace.
    const py::Object sandbox_dict(PyDict_Copy(py_global_dict));
    if (!sandbox_dict) {
      PyErr_Clear();
      return;
    }

    if (PyDict_SetItemString(sandbox_dict.Ptr(), "code", py_code_content.Ptr()) != 0) {
      PyErr_Clear();
      return;
    }

    const py::Object py_run_res(PyRun_String(func_code, Py_file_input, sandbox_dict.Ptr(), sandbox_dict.Ptr()));
    if (!py_run_res) {
      if (auto exc = py::FetchError()) {
        spdlog::warn("Python dependency scan failed for {}: {}", file_path_.string(), py::FormatException(*exc));
      }
      return;
    }

    const py::Object py_res(py::Object::FromBorrow(PyDict_GetItemString(sandbox_dict.Ptr(), "modules")));
    if (!py_res) {
      // PyDict_GetItemString does not set an exception if the key is not found,
      // but it might have failed for other reasons.
      PyErr_Clear();
      return;
    }

    const py::Object iterator(PyObject_GetIter(py_res.Ptr()));
    if (!iterator) {
      PyErr_Clear();
      return;
    }

    PyObject *module_ptr = nullptr;
    while ((module_ptr = PyIter_Next(iterator.Ptr()))) {
      const py::Object module(module_ptr);
      const char *module_name = PyUnicode_AsUTF8(module.Ptr());
      if (!module_name) {
        PyErr_Clear();
        continue;
      }

      const std::string_view module_name_str(module_name);

      const py::Object sys_keys(PyDict_Keys(sys_mod_ref));
      if (!sys_keys) {
        PyErr_Clear();
        break;
      }

      const py::Object sys_iterator(PyObject_GetIter(sys_keys.Ptr()));
      if (!sys_iterator) {
        PyErr_Clear();
        break;
      }

      PyObject *sys_mod_key_ptr = nullptr;
      while ((sys_mod_key_ptr = PyIter_Next(sys_iterator.Ptr()))) {
        const py::Object sys_mod_key(sys_mod_key_ptr);
        const char *sys_mod_key_name = PyUnicode_AsUTF8(sys_mod_key.Ptr());
        if (!sys_mod_key_name) {
          PyErr_Clear();
          continue;
        }

        const std::string_view sys_mod_key_name_str(sys_mod_key_name);
        const bool matches_dep = sys_mod_key_name_str == module_name_str ||
                                 (sys_mod_key_name_str.starts_with(module_name_str) &&
                                  sys_mod_key_name_str.size() > module_name_str.size() &&
                                  sys_mod_key_name_str[module_name_str.size()] == '.');
        const bool is_builtin_bridge = sys_mod_key_name_str == "mgp" || sys_mod_key_name_str == "_mgp" ||
                                       sys_mod_key_name_str.starts_with("mgp.");
        if (matches_dep && sys_mod_key_name_str != module_path && !is_builtin_bridge) {
          if (PyDict_DelItemString(sys_mod_ref, sys_mod_key_name) != 0) {
            spdlog::warn("Failed to remove stale sys.modules entry '{}' during module cleanup", sys_mod_key_name);
            PyErr_Clear();
          }
        }
      }
      if (PyErr_Occurred()) {
        PyErr_Clear();
      }
    }
  }
  if (PyErr_Occurred()) {
    PyErr_Clear();
  }
}

const std::map<std::string, mgp_proc, std::less<>> *PythonModule::Procedures() const {
  MG_ASSERT(py_module_,
            "Attempting to access procedures of a module that has "
            "not been loaded...");
  return &procedures_;
}

const std::map<std::string, mgp_trans, std::less<>> *PythonModule::Transformations() const {
  MG_ASSERT(py_module_,
            "Attempting to access procedures of a module that has "
            "not been loaded...");
  return &transformations_;
}

const std::map<std::string, mgp_func, std::less<>> *PythonModule::Functions() const {
  MG_ASSERT(py_module_,
            "Attempting to access functions of a module that has "
            "not been loaded...");
  return &functions_;
}
#endif  // MG_PYTHON_SUPPORT

namespace {

#ifdef MG_ENTERPRISE
constexpr std::array<const char *, 6> kEnterpriseModuleList = {
    "betweenness_centrality_online",
    "community_detection_online",
    "cross_database",
    "katz_centrality_online",
    "node2vec_online",
    "pagerank_online",
};
#endif

std::unique_ptr<Module> LoadModuleFromFile(const std::filesystem::path &path) {
  const auto &ext = path.extension();
  if (ext != ".so" && ext != ".py") {
    spdlog::warn(utils::MessageWithLink("Unknown query module file {}.", path, "https://memgr.ph/modules"));
    return nullptr;
  }
#ifdef MG_ENTERPRISE
  const auto name = path.stem().string();
  if (!memgraph::license::global_license_checker.IsEnterpriseValidFast() &&
      std::ranges::contains(kEnterpriseModuleList, name)) {
    spdlog::warn(fmt::format("Failed to load query module {} because it requires a valid enterprise license.", path));
    return nullptr;
  }
#endif
  std::unique_ptr<Module> module;
  if (path.extension() == ".so") {
    auto lib_module = std::make_unique<SharedLibraryModule>();
    if (!lib_module->Load(path)) return nullptr;
    module = std::move(lib_module);
  } else if (path.extension() == ".py") {
#ifdef MG_PYTHON_SUPPORT
    auto py_module = std::make_unique<PythonModule>();
    if (!py_module->Load(path)) return nullptr;
    module = std::move(py_module);
#else
    spdlog::warn(utils::MessageWithLink(
        "Unable to load module {}; this Memgraph was built without Python support.", path, "https://memgr.ph/modules"));
    return nullptr;
#endif
  }
  return module;
}

}  // namespace

bool ModuleRegistry::TryEraseModule(std::string_view name) {
  auto it = modules_.find(name);
  if (it == modules_.end()) {
    return false;
  }

  auto &module = it->second;
  if (module.use_count() != 1) {
    return false;
  }

  modules_.erase(it);
  ++generation_;
  return true;
}

bool ModuleRegistry::TryEraseAllModules() {
  auto const any_used =
      ranges::any_of(modules_ | ranges::views::values, [](auto const &module) { return module.use_count() != 1; });
  if (any_used) {
    spdlog::warn("At least one module was still in use");
    return false;
  }

  modules_.clear();
  ++generation_;
  return true;
}

bool ModuleRegistry::AreAnyCustomModulesInUse() const {
  for (const auto &[mod_name, module] : modules_) {
    if (mod_name == "mg") continue;
    if (module.use_count() != 1) {
      return true;
    }
  }
  return false;
}

bool ModuleRegistry::RegisterModule(const std::string_view name, std::unique_ptr<Module> module) {
  MG_ASSERT(!name.empty(), "Module name cannot be empty");
  MG_ASSERT(module, "Tried to register an invalid module");
  if (ReservedBuiltInModuleNames().contains(name)) {
    spdlog::error(utils::MessageWithLink("Unable to overwrite a builtin module {}.", name, "https://memgr.ph/modules"));
    return false;
  }
  if (modules_.contains(name)) {
    spdlog::error(
        utils::MessageWithLink("Unable to overwrite an already loaded module {}.", name, "https://memgr.ph/modules"));
    return false;
  }

  modules_.emplace(std::string(name), std::move(module));
  ++generation_;
  return true;
}

void ModuleRegistry::DoUnloadAllModules() {
  auto mg_it = modules_.find("mg");
  MG_ASSERT(mg_it != modules_.end(), "Expected the builtin \"mg\" module to be present.");
  // This is correct because the destructor will close each module. However,
  // we don't want to unload the builtin "mg" module.

  auto mg_preserved = mg_it->second;

  modules_.erase(mg_it);
  auto on_exit = utils::OnScopeExit{[&] { modules_.emplace("mg", std::move(mg_preserved)); }};

  if (!TryEraseAllModules()) throw query::QueryException("Unable to unload modules, they are currently being used.");
}

ModuleRegistry::ModuleRegistry() {
  auto module = std::make_unique<BuiltinModule>();
  RegisterMgProcedures(&modules_, &lock_, module.get());
  RegisterMgTransformations(&modules_, &lock_, module.get());
  RegisterMgFunctions(&modules_, &lock_, module.get());
  RegisterMgLoad(this, module.get());
  RegisterMgGetModuleFiles(this, module.get());
  RegisterMgGetModuleFile(this, module.get());
  RegisterMgCreateModuleFile(this, module.get());
  RegisterMgUpdateModuleFile(this, module.get());
  RegisterMgDeleteModuleFile(this, module.get());
  modules_.emplace("mg", std::move(module));
}

void ModuleRegistry::SetModulesDirectory(std::vector<std::filesystem::path> modules_dirs,
                                         const std::filesystem::path &data_directory) {
  internal_module_dir_ = data_directory / "internal_modules";
  utils::EnsureDirOrDie(internal_module_dir_);
  modules_dirs_ = std::move(modules_dirs);
  modules_dirs_.push_back(internal_module_dir_);
}

const std::vector<std::filesystem::path> &ModuleRegistry::GetModulesDirectory() const { return modules_dirs_; }

bool ModuleRegistry::LoadModuleIfFound(const std::filesystem::path &modules_dir, const std::string_view name) {
  if (!utils::DirExists(modules_dir)) {
    spdlog::error(
        utils::MessageWithLink("Module directory {} doesn't exist.", modules_dir, "https://memgr.ph/modules"));
    return false;
  }
  for (const auto &entry : std::filesystem::directory_iterator(modules_dir)) {
    const auto &path = entry.path();
    if (entry.is_regular_file() && path.stem() == name) {
      auto module = LoadModuleFromFile(path);
      if (!module) return false;
      return RegisterModule(name, std::move(module));
    }
  }
  return false;
}

bool ModuleRegistry::LoadOrReloadModuleFromName(const std::string_view name) {
  if (modules_dirs_.empty()) return false;
  if (name.empty()) return false;
  auto guard = std::unique_lock{lock_};

  if (ReservedBuiltInModuleNames().contains(name)) {
    return false;
  }

  auto existing_it = modules_.find(name);
  if (existing_it != modules_.end() && existing_it->second.use_count() != 1) {
    throw query::QueryException(
        "Unable to unload module '{}', it is currently being used. In order to check "
        "whether the module exists, please use CALL mg.procedures() YIELD * for custom query procedures, or CALL "
        "mg.functions() YIELD * for custom query functions.",
        name);
  }

  std::optional<std::filesystem::path> candidate_path;
  for (const auto &module_dir : modules_dirs_) {
    if (!utils::DirExists(module_dir)) {
      spdlog::error(
          utils::MessageWithLink("Module directory {} doesn't exist.", module_dir, "https://memgr.ph/modules"));
      continue;
    }
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(module_dir, ec)) {
      std::error_code file_ec;
      if (entry.is_regular_file(file_ec) && !file_ec && entry.path().stem() == name &&
          IsLoadableModuleExtension(entry.path().extension())) {
        candidate_path = entry.path();
        break;
      }
    }
    if (candidate_path) break;
  }

  if (!candidate_path) {
    return false;
  }

  auto new_module = LoadModuleFromFile(*candidate_path);
  if (!new_module) {
    return false;
  }

  if (existing_it != modules_.end()) {
    existing_it->second = std::move(new_module);
    ++generation_;
    return true;
  }
  return RegisterModule(name, std::move(new_module));
}

std::vector<std::filesystem::path> ModuleRegistry::LoadModulesFromDirectory(const std::filesystem::path &modules_dir) {
  std::vector<std::filesystem::path> failed_paths;
  // all modules except mg are deleted before this
  if (modules_dir.empty()) return failed_paths;
  if (!utils::DirExists(modules_dir)) {
    spdlog::error(
        utils::MessageWithLink("Module directory {} doesn't exist.", modules_dir, "https://memgr.ph/modules"));
    return failed_paths;
  }
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(modules_dir, ec)) {
    const auto &path = entry.path();
    std::error_code file_ec;
    if (entry.is_regular_file(file_ec) && !file_ec) {
      if (!IsLoadableModuleExtension(path.extension())) continue;
      std::string name = path.stem();
      if (name.empty()) continue;
      auto module = LoadModuleFromFile(path);
      if (!module) {
        failed_paths.push_back(path);
        continue;
      }
      if (!RegisterModule(name, std::move(module))) {
        failed_paths.push_back(path);
      }
    }
  }
  return failed_paths;
}

void ModuleRegistry::UnloadAndLoadModulesFromDirectories() {
  auto guard = std::unique_lock{lock_};
  DoUnloadAllModules();
  for (const auto &module_dir : modules_dirs_) {
    static_cast<void>(LoadModulesFromDirectory(module_dir));
  }
}

std::expected<std::string, std::string> ModuleRegistry::ReadModuleFileContent(const std::filesystem::path &path) const {
  auto guard = std::shared_lock{lock_};
  if (!path.is_absolute()) {
    return std::unexpected("The path should be an absolute path.");
  }
  if (!IsAllowedExtension(path.extension())) {
    return std::unexpected("The specified file isn't in the supported format.");
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::unexpected("The specified file doesn't exist.");
  }
  if (auto maybe_parent = ParentModuleDirectory(*this, path); !maybe_parent.has_value()) {
    return std::unexpected(maybe_parent.error());
  }
  const auto maybe_content = ReadFile(path);
  if (!maybe_content) {
    return std::unexpected("Couldn't read the content of the file.");
  }
  return *maybe_content;
}

std::expected<std::filesystem::path, std::string> ModuleRegistry::CreateModuleFile(std::string_view filename,
                                                                                   std::string_view content) {
  auto guard = std::unique_lock{lock_};
  const auto file_path = internal_module_dir_ / filename;
  if (!IsSubPath(internal_module_dir_, file_path)) {
    return std::unexpected(
        "Invalid relative path defined. The module file cannot be define outside the internal modules directory.");
  }
  if (!IsAllowedExtension(file_path.extension())) {
    return std::unexpected("The specified file isn't in the supported format.");
  }
  if (AreAnyCustomModulesInUse()) {
    return std::unexpected("Unable to unload modules, they are currently being used.");
  }

  std::vector<std::filesystem::path> missing_dirs;
  for (auto p = file_path.parent_path(); !p.empty() && p != internal_module_dir_; p = p.parent_path()) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) break;
    missing_dirs.push_back(p);
  }
  utils::EnsureDir(file_path.parent_path());

  if (auto write_res = CreateFileExclusive(file_path, content); !write_res.has_value()) {
    for (const auto &d : missing_dirs) {
      std::error_code ec;
      std::filesystem::remove(d, ec);
    }
    return std::unexpected(std::move(write_res.error()));
  }

  std::error_code canon_ec;
  auto canonical_created = std::filesystem::canonical(file_path, canon_ec);
  if (canon_ec) {
    canonical_created = file_path;
  }

  DoUnloadAllModules();
  std::vector<std::filesystem::path> failed_paths;
  for (const auto &module_dir : modules_dirs_) {
    auto dir_failures = LoadModulesFromDirectory(module_dir);
    failed_paths.insert(failed_paths.end(), dir_failures.begin(), dir_failures.end());
  }

  const bool created_failed = std::ranges::any_of(failed_paths, [&](const auto &failed) {
    std::error_code eq_ec;
    return std::filesystem::equivalent(failed, file_path, eq_ec) && !eq_ec;
  });
  if (created_failed || !failed_paths.empty()) {
    std::error_code rm_ec;
    std::filesystem::remove(file_path, rm_ec);
    for (const auto &d : missing_dirs) {
      std::error_code dir_ec;
      std::filesystem::remove(d, dir_ec);
    }
    DoUnloadAllModules();
    for (const auto &module_dir : modules_dirs_) {
      static_cast<void>(LoadModulesFromDirectory(module_dir));
    }
    return std::unexpected(
        fmt::format("Failed to load module after creating file {}.", canonical_created.generic_string()));
  }

  return canonical_created;
}

std::expected<void, std::string> ModuleRegistry::UpdateModuleFile(const std::filesystem::path &path,
                                                                  std::string_view content) {
  auto guard = std::unique_lock{lock_};
  if (!path.is_absolute()) {
    return std::unexpected("The path should be an absolute path.");
  }
  if (!IsAllowedExtension(path.extension())) {
    return std::unexpected("The specified file isn't in the supported format.");
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::unexpected("The specified file doesn't exist.");
  }
  if (auto maybe_parent = ParentModuleDirectory(*this, path); !maybe_parent.has_value()) {
    return std::unexpected(maybe_parent.error());
  }
  if (AreAnyCustomModulesInUse()) {
    return std::unexpected("Unable to unload modules, they are currently being used.");
  }

  const auto previous_content = ReadFile(path);
  if (!previous_content) {
    return std::unexpected("Couldn't read the content of the file.");
  }

  if (auto write_res = WriteToFileAtomic(path, content); !write_res.has_value()) {
    return std::unexpected(std::move(write_res.error()));
  }

  DoUnloadAllModules();
  std::vector<std::filesystem::path> failed_paths;
  for (const auto &module_dir : modules_dirs_) {
    auto dir_failures = LoadModulesFromDirectory(module_dir);
    failed_paths.insert(failed_paths.end(), dir_failures.begin(), dir_failures.end());
  }

  if (!failed_paths.empty()) {
    static_cast<void>(WriteToFileAtomic(path, *previous_content));
    DoUnloadAllModules();
    for (const auto &module_dir : modules_dirs_) {
      static_cast<void>(LoadModulesFromDirectory(module_dir));
    }
    return std::unexpected(fmt::format("Failed to reload module after updating file {}.", path.generic_string()));
  }
  return {};
}

std::expected<void, std::string> ModuleRegistry::DeleteModuleFile(const std::filesystem::path &path) {
  auto guard = std::unique_lock{lock_};
  if (!path.is_absolute()) {
    return std::unexpected("The path should be an absolute path.");
  }
  if (!IsAllowedExtension(path.extension())) {
    return std::unexpected("The specified file isn't in the supported format.");
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::unexpected("The specified file doesn't exist.");
  }
  const auto parent_module_directory = ParentModuleDirectory(*this, path);
  if (!parent_module_directory) {
    return std::unexpected(parent_module_directory.error());
  }
  if (AreAnyCustomModulesInUse()) {
    return std::unexpected("Unable to unload modules, they are currently being used.");
  }

  if (!std::filesystem::remove(path, ec)) {
    return std::unexpected(fmt::format("Failed to delete the module: {}", ec.message()));
  }

  auto parent_path = path.parent_path();
  std::error_code cleanup_ec;
  while (!std::filesystem::is_symlink(parent_path, cleanup_ec) && !cleanup_ec &&
         std::filesystem::is_empty(parent_path, cleanup_ec) && !cleanup_ec &&
         !std::filesystem::equivalent(*parent_module_directory, parent_path, cleanup_ec) && !cleanup_ec) {
    std::filesystem::remove(parent_path, cleanup_ec);
    if (cleanup_ec) break;
    parent_path = parent_path.parent_path();
  }

  DoUnloadAllModules();
  for (const auto &module_dir : modules_dirs_) {
    static_cast<void>(LoadModulesFromDirectory(module_dir));
  }
  return {};
}

/// Takes a read lock.
auto ModuleRegistry::GetModuleNamed(const std::string_view name) const -> std::shared_ptr<Module> {
  auto guard = std::shared_lock{lock_};
  auto it = modules_.find(name);

  if (it == modules_.end()) {
    return {};
  }
  return it->second;
}

void ModuleRegistry::UnloadAllModules() {
  auto guard = std::unique_lock{lock_};
  DoUnloadAllModules();
}

utils::MemoryResource &ModuleRegistry::GetSharedMemoryResource() noexcept { return *shared_; }

bool ModuleRegistry::RegisterMgProcedure(const std::string_view name, mgp_proc proc) {
  auto guard = std::unique_lock{lock_};
  if (auto module_it = modules_.find("mg"); module_it != modules_.end()) {
    auto *builtin_module = dynamic_cast<BuiltinModule *>(module_it->second.get());
    builtin_module->AddProcedure(name, std::move(proc));
    return true;
  }
  return false;
}

const std::filesystem::path &ModuleRegistry::InternalModuleDir() const noexcept { return internal_module_dir_; }

namespace {

/// This function returns a pair of either
//      ModuleName | Prop
/// 1. <ModuleName,  ProcedureName>
/// 2. <ModuleName,  TransformationName>
auto FindModuleNameAndProp(std::string_view fully_qualified_name)
    -> std::optional<std::pair<std::string_view, std::string_view>> {
  auto last_dot_pos = fully_qualified_name.find_last_of('.');
  if (last_dot_pos == std::string_view::npos || last_dot_pos == 0 ||
      last_dot_pos + 1 >= fully_qualified_name.size()) {
    return std::nullopt;
  }
  auto module_name = fully_qualified_name.substr(0, last_dot_pos);
  auto name = fully_qualified_name.substr(last_dot_pos + 1);
  return std::pair{module_name, name};
}

template <typename T>
concept ModuleProperties = utils::SameAsAnyOf<T, mgp_proc, mgp_trans, mgp_func>;

template <ModuleProperties T>
auto MakePairIfPropFound(const ModuleRegistry &module_registry, std::string_view fully_qualified_name)
    -> find_result<T> {
  auto prop_fun = [](auto &module) {
    if constexpr (std::is_same_v<T, mgp_proc>) {
      return module->Procedures();
    } else if constexpr (std::is_same_v<T, mgp_trans>) {
      return module->Transformations();
    } else if constexpr (std::is_same_v<T, mgp_func>) {
      return module->Functions();
    }
  };
  auto result = FindModuleNameAndProp(fully_qualified_name);
  if (!result) {
    return std::nullopt;
  }
  auto [module_name, module_prop_name] = *result;
  auto module = module_registry.GetModuleNamed(module_name);
  std::string prop_name;
  if (module) {
    prop_name = std::string(module_prop_name);
  } else {
    // Check for possible callable aliases.
    const auto maybe_valid_alias = gCallableAliasMapper.FindAlias(fully_qualified_name);
    if (maybe_valid_alias) {
      result = FindModuleNameAndProp(*maybe_valid_alias);
      if (!result) {
        return std::nullopt;
      }
      auto [alias_module_name, alias_prop_name] = *result;
      module = module_registry.GetModuleNamed(alias_module_name);
      if (!module) {
        return std::nullopt;
      }
      prop_name = std::string(alias_prop_name);
    } else {
      return std::nullopt;
    }
  }
  auto *prop = prop_fun(module);
  const auto &prop_it = prop->find(prop_name);
  if (prop_it == prop->end()) return std::nullopt;
  return std::make_pair(std::move(module), &prop_it->second);
}

}  // namespace

auto FindProcedure(const ModuleRegistry &module_registry, std::string_view fully_qualified_procedure_name)
    -> find_result<mgp_proc> {
  return MakePairIfPropFound<mgp_proc>(module_registry, fully_qualified_procedure_name);
}

auto FindTransformation(const ModuleRegistry &module_registry, std::string_view fully_qualified_transformation_name)
    -> find_result<mgp_trans> {
  return MakePairIfPropFound<mgp_trans>(module_registry, fully_qualified_transformation_name);
}

auto FindFunction(const ModuleRegistry &module_registry, std::string_view fully_qualified_function_name)
    -> find_result<mgp_func> {
  return MakePairIfPropFound<mgp_func>(module_registry, fully_qualified_function_name);
}

void ConstructArguments(std::span<TypedValue const> args, mgp_func const &callable, mgp_list &args_list,
                        mgp_graph &graph) {
  const auto n_args = args.size();
  const auto c_args_sz = callable.args.size();
  const auto c_opt_args_sz = callable.opt_args.size();

  args_list.elems.reserve(c_args_sz + c_opt_args_sz);

  // Copy provided args
  for (size_t i = 0; i < n_args; ++i) {
    args_list.elems.emplace_back(args[i], &graph);
  }
  // Fill missing optional arguments with their default values.
  const size_t passed_in_opt_args = n_args - c_args_sz;
  for (size_t i = passed_in_opt_args; i < c_opt_args_sz; ++i) {
    args_list.elems.emplace_back(std::get<2>(callable.opt_args[i]), &graph);
  }
}

void ConstructArguments(std::span<TypedValue const> args, mgp_proc const &callable, mgp_list &args_list,
                        mgp_graph &graph) {
  const auto n_args = args.size();
  const auto c_args_sz = callable.args.size();
  const auto c_opt_args_sz = callable.opt_args.size();

  args_list.elems.reserve(c_args_sz + c_opt_args_sz);

  // Copy provided args
  for (size_t i = 0; i < n_args; ++i) {
    args_list.elems.emplace_back(args[i], &graph);
  }
  // Fill missing optional arguments with their default values.
  const size_t passed_in_opt_args = n_args - c_args_sz;
  for (size_t i = passed_in_opt_args; i < c_opt_args_sz; ++i) {
    args_list.elems.emplace_back(std::get<2>(callable.opt_args[i]), &graph);
  }
}

}  // namespace memgraph::query::procedure
