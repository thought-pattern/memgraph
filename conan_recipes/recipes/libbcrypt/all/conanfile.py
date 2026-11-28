import glob
import os
import shlex

from conan import ConanFile
from conan.tools.apple import is_apple_os
from conan.tools.files import (
    apply_conandata_patches,
    chdir,
    copy,
    export_conandata_patches,
    get,
    rename,
    replace_in_file,
    trim_conandata,
)
from conan.tools.gnu import AutotoolsToolchain
from conan.tools.layout import basic_layout


class LibbcryptConan(ConanFile):
    name = "libbcrypt"
    version = "1.0-memgraph"
    description = "bcrypt password hashing library."
    url = "https://github.com/memgraph/memgraph"
    license = "ISC"
    homepage = "https://github.com/rg3/libbcrypt"
    topics = ("bcrypt", "password", "hashing")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        basic_layout(self, src_folder="src")

    def export(self):
        # Stabilise recipe revision across conan export and local-recipes-index
        trim_conandata(self)

    def export_sources(self):
        export_conandata_patches(self)

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        apply_conandata_patches(self)

    def _collect_toolchain_flags(self):
        tc = AutotoolsToolchain(self)
        tc_vars = tc.vars()
        cflags_parts = []
        ldflags_parts = []
        for var_name in ("CPPFLAGS", "CFLAGS"):
            val = tc_vars.get(var_name)
            if val:
                cflags_parts.append(val)
        ld_val = tc_vars.get("LDFLAGS")
        if ld_val:
            ldflags_parts.append(ld_val)

        if self.settings.os != "Windows" and (self.options.shared or self.options.get_safe("fPIC")):
            joined_c = " ".join(cflags_parts)
            if "-fPIC" not in joined_c:
                cflags_parts.append("-fPIC")

        sanitizers = (
            ("compiler.asan", "-fsanitize=address"),
            ("compiler.ubsan", "-fsanitize=undefined"),
            ("compiler.tsan", "-fsanitize=thread"),
        )
        for setting_key, flag in sanitizers:
            if self.settings.get_safe(setting_key):
                if flag not in " ".join(cflags_parts):
                    cflags_parts.append(flag)
                if flag not in " ".join(ldflags_parts):
                    ldflags_parts.append(flag)

        return " ".join(cflags_parts).strip(), " ".join(ldflags_parts).strip()

    def _shared_lib_filename(self):
        if self.settings.os == "Windows":
            return "bcrypt.dll"
        if is_apple_os(self):
            return "libbcrypt.dylib"
        return "libbcrypt.so"

    def generate(self):
        tc = AutotoolsToolchain(self)
        tc.generate()

    def build(self):
        top_makefile = os.path.join(self.source_folder, "Makefile")
        crypt_blowfish_makefile = os.path.join(self.source_folder, "crypt_blowfish", "Makefile")
        replace_in_file(self, crypt_blowfish_makefile, "-Wcast-align", "", strict=False)

        for mk_path in (top_makefile, crypt_blowfish_makefile):
            with open(mk_path, "r", encoding="utf-8") as mf:
                mk_text = mf.read()
            if "CONAN_EXTRA_CFLAGS" not in mk_text:
                with open(mk_path, "a", encoding="utf-8") as mf:
                    mf.write("\nCFLAGS += $(CONAN_EXTRA_CFLAGS)\nLDFLAGS += $(CONAN_EXTRA_LDFLAGS)\n")

        extra_cflags, extra_ldflags = self._collect_toolchain_flags()
        make = self.conf.get("tools.gnu:make_program", default="make")
        cc = self.conf.get("tools.build:compiler_executables", default={}).get("c", "cc")

        with chdir(self, self.source_folder):
            cmd = (
                f"{shlex.quote(make)} "
                f"CC={shlex.quote(cc)} "
                f"CONAN_EXTRA_CFLAGS={shlex.quote(extra_cflags)} "
                f"CONAN_EXTRA_LDFLAGS={shlex.quote(extra_ldflags)}"
            )
            self.run(cmd)

            if self.options.shared:
                shared_name = self._shared_lib_filename()
                objs = ["bcrypt.o"] + sorted(glob.glob(os.path.join("crypt_blowfish", "*.o")))
                quoted_objs = " ".join(shlex.quote(obj) for obj in objs)
                soname_flag = ""
                if is_apple_os(self):
                    soname_flag = f"-Wl,-install_name,@rpath/{shared_name}"
                elif self.settings.os != "Windows":
                    soname_flag = f"-Wl,-soname,{shared_name}"
                link_parts = [shlex.quote(cc), "-shared"]
                if soname_flag:
                    link_parts.append(shlex.quote(soname_flag))
                if extra_cflags:
                    link_parts.append(extra_cflags)
                link_parts.append(quoted_objs)
                if extra_ldflags:
                    link_parts.append(extra_ldflags)
                link_parts.extend(["-o", shlex.quote(shared_name)])
                self.run(" ".join(link_parts))

    def package(self):
        copy(self, "COPYING", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        if self.options.shared:
            shared_name = self._shared_lib_filename()
            dst_sub = "bin" if self.settings.os == "Windows" else "lib"
            copy(self, shared_name, src=self.source_folder, dst=os.path.join(self.package_folder, dst_sub))
        else:
            copy(self, "bcrypt.a", src=self.source_folder, dst=os.path.join(self.package_folder, "lib"))
            rename(
                self,
                os.path.join(self.package_folder, "lib", "bcrypt.a"),
                os.path.join(self.package_folder, "lib", "libbcrypt.a"),
            )
        copy(self, "bcrypt.h", src=self.source_folder, dst=os.path.join(self.package_folder, "include"))

    def package_info(self):
        self.cpp_info.libs = ["bcrypt"]
