from typing import Any

Property = Any
VertexId = int
EdgeId = int
Label = str


class MgpIterable:
    def get(self) -> Any:
        pass

    def next(self) -> Any:
        pass


class Vertex:
    def is_valid(self) -> bool:  # type: ignore
        pass

    def underlying_graph_is_mutable(self) -> bool:  # type: ignore
        pass

    def iter_properties(self) -> MgpIterable:  # type: ignore
        pass

    def get_property(self, property_name: str) -> "Property":  # type: ignore
        pass

    def set_property(self, property_name: str, value: Any) -> "Property":  # type: ignore
        pass

    def set_properties(self, properties: dict) -> None:
        pass

    def get_id(self) -> "VertexId":  # type: ignore
        pass

    def label_at(self, index: int) -> "Label":  # type: ignore
        pass

    def labels_count(self) -> int:  # type: ignore
        pass

    def add_label(self, label: Any):
        pass

    def remove_label(self, label: Any):
        pass

    def iter_in_edges(self) -> MgpIterable:  # type: ignore
        pass

    def iter_out_edges(self) -> MgpIterable:  # type: ignore
        pass


class Edge:
    def is_valid(self) -> bool:  # type: ignore
        pass

    def underlying_graph_is_mutable(self) -> bool:  # type: ignore
        pass

    def iter_properties(self) -> MgpIterable:  # type: ignore
        pass

    def get_property(self, property_name: str) -> "Property":  # type: ignore
        pass

    def set_property(self, property_name: str, value: Any) -> "Property":  # type: ignore
        pass

    def set_properties(self, properties: dict) -> None:
        pass

    def get_type_name(self) -> str:  # type: ignore
        pass

    def get_id(self) -> "EdgeId":  # type: ignore
        pass

    def from_vertex(self) -> Vertex:  # type: ignore
        pass

    def to_vertex(self) -> Vertex:  # type: ignore
        pass


class Path:
    def is_valid(self) -> bool:  # type: ignore
        pass

    @staticmethod
    def make_with_start(vertex: Vertex) -> "Path":  # type: ignore
        pass

    def expand(self, edge: Edge) -> None:
        pass

    def pop(self) -> None:
        pass

    def size(self) -> int:  # type: ignore
        pass

    def vertex_at(self, index: int) -> Vertex:  # type: ignore
        pass

    def edge_at(self, index: int) -> Edge:  # type: ignore
        pass


class Graph:
    def is_valid(self) -> bool:  # type: ignore
        pass

    def is_mutable(self) -> bool:  # type: ignore
        pass

    def start_timestamp(self) -> int:  # type: ignore
        pass

    def get_vertex_by_id(self, vertex_id: int) -> Vertex:  # type: ignore
        pass

    def iter_vertices(self) -> MgpIterable:  # type: ignore
        pass

    def create_vertex(self) -> Vertex:  # type: ignore
        pass

    def create_edge(self, from_vertex: Vertex, to_vertex: Vertex, edge_type: str) -> Edge:  # type: ignore
        pass

    def delete_vertex(self, vertex: Vertex) -> None:
        pass

    def detach_delete_vertex(self, vertex: Vertex) -> None:
        pass

    def delete_edge(self, edge: Edge) -> None:
        pass


class CypherType:
    pass


class Message:
    def is_valid(self) -> bool:  # type: ignore
        pass

    def source_type(self) -> str:  # type: ignore
        pass

    def topic_name(self) -> str:  # type: ignore
        pass

    def key(self) -> bytes:  # type: ignore
        pass

    def timestamp(self) -> int:  # type: ignore
        pass

    def offset(self) -> int:  # type: ignore
        pass

    def payload(self) -> bytes:  # type: ignore
        pass


class Messages:
    def is_valid(self) -> bool:  # type: ignore
        pass

    def message_at(self, id: int) -> Message:  # type: ignore
        pass

    def total_messages(self) -> int:  # type: ignore
        pass


class UnknownError(Exception):
    pass


class UnableToAllocateError(Exception):
    pass


class InsufficientBufferError(Exception):
    pass


class OutOfRangeError(Exception):
    pass


class LogicErrorError(Exception):
    pass


class DeletedObjectError(Exception):
    pass


class InvalidArgumentError(Exception):
    pass


class KeyAlreadyExistsError(Exception):
    pass


class ImmutableObjectError(Exception):
    pass


class ValueConversionError(Exception):
    pass


class SerializationError(Exception):
    pass


class AuthorizationError(Exception):
    pass


def type_nullable(elem: Any):
    return CypherType()


def type_list(elem: Any):
    return CypherType()


def type_bool():
    return CypherType()


def type_string():
    return CypherType()


def type_int():
    return CypherType()


def type_float():
    return CypherType()


def type_number():
    return CypherType()


def type_map():
    return CypherType()


def type_node():
    return CypherType()


def type_relationship():
    return CypherType()


def type_path():
    return CypherType()


def type_date():
    return CypherType()


def type_local_time():
    return CypherType()


def type_local_date_time():
    return CypherType()


def type_duration():
    return CypherType()


def type_zoned_date_time():
    return CypherType()


def type_point_2d():
    return CypherType()


def type_point_3d():
    return CypherType()


def type_enum():
    return CypherType()


def type_any():
    return CypherType()


class Procedure:
    def add_arg(self, name: str, cypher_type: Any) -> None:
        pass

    def add_opt_arg(self, name: str, cypher_type: Any, default: Any = None) -> None:
        pass

    def add_result(self, name: str, cypher_type: Any) -> None:
        pass

    def add_deprecated_result(self, name: str, cypher_type: Any) -> None:
        pass


class Function:
    def add_arg(self, name: str, cypher_type: Any) -> None:
        pass

    def add_opt_arg(self, name: str, cypher_type: Any, default: Any = None) -> None:
        pass


class Module:
    def add_read_procedure(self, wrapper: Any) -> Procedure:
        return Procedure()

    def add_write_procedure(self, wrapper: Any) -> Procedure:
        return Procedure()

    def add_batch_read_procedure(self, wrapper: Any, initializer: Any, cleanup: Any) -> Procedure:
        return Procedure()

    def add_batch_write_procedure(self, wrapper: Any, initializer: Any, cleanup: Any) -> Procedure:
        return Procedure()

    def add_transformation(self, wrapper: Any) -> None:
        pass

    def add_function(self, wrapper: Any) -> Function:
        return Function()


_MODULE = Module()


class Utils:
    @staticmethod
    def is_enterprise_valid() -> bool:
        return False


class Logger:
    def info(self, out: str) -> None:
        pass

    def warning(self, out: str) -> None:
        pass

    def critical(self, out: str) -> None:
        pass

    def error(self, out: str) -> None:
        pass

    def trace(self, out: str) -> None:
        pass

    def debug(self, out: str) -> None:
        pass


_LOGGER = Logger()

SOURCE_TYPE_KAFKA = "SOURCE_TYPE_KAFKA"
SOURCE_TYPE_PULSAR = "SOURCE_TYPE_PULSAR"
