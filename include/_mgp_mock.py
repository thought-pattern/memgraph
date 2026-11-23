import typing
from enum import Enum

import networkx as nx

NX_LABEL_ATTR = "labels"
NX_TYPE_ATTR = "type"

SOURCE_TYPE_KAFKA = "SOURCE_TYPE_KAFKA"
SOURCE_TYPE_PULSAR = "SOURCE_TYPE_PULSAR"

"""
This module provides helpers for the mock Python API, much like _mgp.py does for mgp.py.
"""


class InvalidArgumentError(Exception):
    """
    Signals that some of the arguments have invalid values.
    """

    pass


class ImmutableObjectError(Exception):
    pass


class OutOfRangeError(Exception):
    pass


class LogicErrorError(Exception):
    pass


class DeletedObjectError(Exception):
    pass


class EdgeConstants(Enum):
    I_START = 0
    I_END = 1
    I_KEY = 2


class Graph:
    """Wrapper around a NetworkX MultiDiGraph instance."""

    __slots__ = ("nx", "_highest_vertex_id", "_highest_edge_id", "_valid", "start_timestamp")

    _next_start_timestamp = 0

    def __init__(self, graph: nx.MultiDiGraph) -> None:
        if not isinstance(graph, nx.MultiDiGraph):
            raise TypeError(f"Expected 'networkx.classes.multidigraph.MultiDiGraph', got '{type(graph)}'")

        self.nx = graph
        self._highest_vertex_id = None
        self._highest_edge_id = None
        self._valid = True
        # Mirror the real Storage::Accessor::GetStartTimestamp contract: a value
        # that is stable for the lifetime of this Graph instance.
        Graph._next_start_timestamp += 1
        self.start_timestamp = Graph._next_start_timestamp

    @property
    def vertex_ids(self):
        return self.nx.nodes

    def vertex_is_isolate(self, vertex_id: int) -> bool:
        return nx.is_isolate(self.nx, vertex_id)

    @property
    def vertices(self):
        return (Vertex(node_id, self) for node_id in self.nx.nodes)

    def has_node(self, node_id):
        return self.nx.has_node(node_id)

    @property
    def edges(self):
        return self.nx.edges

    def is_valid(self) -> bool:
        return self._valid

    def get_vertex_by_id(self, vertex_id: int) -> "Vertex":
        return Vertex(vertex_id, self)

    def invalidate(self):
        self._valid = False

    def is_immutable(self) -> bool:
        return nx.is_frozen(self.nx)

    def make_immutable(self):
        self.nx = nx.freeze(self.nx)

    def _new_vertex_id(self):
        return self.highest_vertex_id + 1

    def _new_edge_id(self):
        return self.highest_edge_id + 1

    def create_vertex(self) -> "Vertex":
        vertex_id = self._new_vertex_id()

        self.nx.add_node(vertex_id)
        self._highest_vertex_id = vertex_id

        return Vertex(vertex_id, self)

    def create_edge(self, from_vertex: "Vertex", to_vertex: "Vertex", edge_type: str) -> "Edge":
        if from_vertex.is_deleted() or to_vertex.is_deleted():
            raise DeletedObjectError("Accessing deleted object.")

        edge_id = self._new_edge_id()

        from_id = from_vertex.id
        to_id = to_vertex.id

        self.nx.add_edge(from_id, to_id, key=edge_id, type=edge_type)
        self._highest_edge_id = edge_id

        return Edge((from_id, to_id, edge_id), self)

    def delete_vertex(self, vertex_id: int):
        _ = self.highest_vertex_id
        self.nx.remove_node(vertex_id)

    def delete_edge(self, from_vertex_id: int, to_vertex_id: int, edge_id: int):
        _ = self.highest_edge_id
        self.nx.remove_edge(from_vertex_id, to_vertex_id, edge_id)

    @property
    def highest_vertex_id(self) -> int:
        if self._highest_vertex_id is None:
            self._highest_vertex_id = max(self.nx.nodes, default=-1)

        return self._highest_vertex_id

    @property
    def highest_edge_id(self) -> int:
        if self._highest_edge_id is None:
            self._highest_edge_id = max(
                (edge[EdgeConstants.I_KEY.value] for edge in self.nx.edges(keys=True)),
                default=-1,
            )

        return self._highest_edge_id


class Vertex:
    """Represents a graph vertex."""

    __slots__ = ("_id", "_graph")

    def __init__(self, id: int, graph: Graph) -> None:
        if not isinstance(id, int):
            raise TypeError(f"Expected 'int', got '{type(id)}'")

        if not isinstance(graph, Graph):
            raise TypeError(f"Expected '_mgp_mock.Graph', got '{type(graph)}'")

        if not graph.nx.has_node(id):
            raise IndexError(f"Unable to find vertex with ID {id}.")

        self._id = id
        self._graph = graph

    def is_valid(self) -> bool:
        return self._graph.is_valid()

    def is_deleted(self) -> bool:
        return not self._graph.nx.has_node(self._id) and self._id <= self._graph.highest_vertex_id

    @property
    def underlying_graph(self) -> Graph:
        return self._graph

    def underlying_graph_is_mutable(self) -> bool:
        return not nx.is_frozen(self._graph.nx)

    @property
    def labels(self) -> typing.List[str]:
        raw = self._graph.nx.nodes[self._id].get(NX_LABEL_ATTR)
        if not raw:
            return []
        if isinstance(raw, str):
            return list(dict.fromkeys(part for part in raw.split(":") if part))
        return list(dict.fromkeys(str(part) for part in raw if part))

    def add_label(self, label: typing.Any) -> None:
        if nx.is_frozen(self._graph.nx):
            raise ImmutableObjectError("Cannot modify immutable object.")

        label_name = label.name if hasattr(label, "name") else str(label)
        current = self.labels
        if label_name and label_name not in current:
            current.append(label_name)
        self._graph.nx.nodes[self._id][NX_LABEL_ATTR] = current

    def remove_label(self, label: typing.Any) -> None:
        if nx.is_frozen(self._graph.nx):
            raise ImmutableObjectError("Cannot modify immutable object.")

        label_name = label.name if hasattr(label, "name") else str(label)
        self._graph.nx.nodes[self._id][NX_LABEL_ATTR] = [item for item in self.labels if item != label_name]

    @property
    def id(self) -> int:
        return self._id

    @property
    def properties(self):
        return (
            (key, value)
            for key, value in self._graph.nx.nodes[self._id].items()
            if key not in (NX_LABEL_ATTR, NX_TYPE_ATTR) and value is not None
        )

    def get_property(self, property_name: str):
        value = self._graph.nx.nodes[self._id][property_name]
        if value is None:
            raise KeyError(property_name)
        return value

    def set_property(self, property_name: str, value: object):
        if value is None:
            self._graph.nx.nodes[self._id].pop(property_name, None)
        else:
            self._graph.nx.nodes[self._id][property_name] = value

    @property
    def in_edges(self) -> typing.Iterable["Edge"]:
        return [Edge(edge, self._graph) for edge in self._graph.nx.in_edges(self._id, keys=True)]

    @property
    def out_edges(self) -> typing.Iterable["Edge"]:
        return [Edge(edge, self._graph) for edge in self._graph.nx.out_edges(self._id, keys=True)]


class Edge:
    """Represents a graph edge."""

    __slots__ = ("_edge", "_graph")

    def __init__(self, edge: typing.Tuple[int, int, int], graph: Graph) -> None:
        if not isinstance(edge, typing.Tuple):
            raise TypeError(f"Expected 'Tuple', got '{type(edge)}'")

        if not isinstance(graph, Graph):
            raise TypeError(f"Expected '_mgp_mock.Graph', got '{type(graph)}'")

        if not graph.nx.has_edge(*edge):
            raise IndexError(f"Unable to find edge with ID {edge[EdgeConstants.I_KEY.value]}.")

        self._edge = edge
        self._graph = graph

    def is_valid(self) -> bool:
        return self._graph.is_valid()

    def is_deleted(self) -> bool:
        return (
            not self._graph.nx.has_edge(*self._edge)
            and self._edge[EdgeConstants.I_KEY.value] <= self._graph.highest_edge_id
        )

    def underlying_graph_is_mutable(self) -> bool:
        return not nx.is_frozen(self._graph.nx)

    @property
    def id(self) -> int:
        return self._edge[EdgeConstants.I_KEY.value]

    @property
    def edge(self) -> typing.Tuple[int, int, int]:
        return self._edge

    @property
    def start_id(self) -> int:
        return self._edge[EdgeConstants.I_START.value]

    @property
    def end_id(self) -> int:
        return self._edge[EdgeConstants.I_END.value]

    def get_type_name(self):
        return self._graph.nx.get_edge_data(*self._edge)[NX_TYPE_ATTR]

    def from_vertex(self) -> Vertex:
        return Vertex(self.start_id, self._graph)

    def to_vertex(self) -> Vertex:
        return Vertex(self.end_id, self._graph)

    @property
    def properties(self):
        return (
            (key, value)
            for key, value in self._graph.nx.edges[self._edge].items()
            if key not in (NX_LABEL_ATTR, NX_TYPE_ATTR) and value is not None
        )

    def get_property(self, property_name: str):
        value = self._graph.nx.edges[self._edge][property_name]
        if value is None:
            raise KeyError(property_name)
        return value

    def set_property(self, property_name: str, value: object):
        if value is None:
            self._graph.nx.edges[self._edge].pop(property_name, None)
        else:
            self._graph.nx.edges[self._edge][property_name] = value


class Path:
    """Represents a path comprised of `Vertex` and `Edge` instances."""

    __slots__ = ("_vertices", "_edges", "_graph")
    __create_key = object()

    def __init__(self, create_key, vertex_id: int, graph: Graph) -> None:
        assert create_key == Path.__create_key, "Path objects must be created using Path.make_with_start"

        self._vertices = [vertex_id]
        self._edges = []
        self._graph = graph

    @classmethod
    def make_with_start(cls, vertex: Vertex) -> "Path":
        if not isinstance(vertex, Vertex):
            raise TypeError(f"Expected 'Vertex', got '{type(vertex)}'")

        if not isinstance(vertex.underlying_graph, Graph):
            raise TypeError(f"Expected '_mgp_mock.Graph', got '{type(vertex.underlying_graph)}'")

        if not vertex.underlying_graph.nx.has_node(vertex._id):
            raise IndexError(f"Unable to find vertex with ID {vertex._id}.")

        return Path(cls.__create_key, vertex._id, vertex.underlying_graph)

    def is_valid(self) -> bool:
        return self._graph.is_valid()

    def underlying_graph_is_mutable(self) -> bool:
        return not nx.is_frozen(self._graph.nx)

    def expand(self, edge: Edge):
        last_vertex = self._vertices[-1]
        if edge.end_id == last_vertex:
            next_vertex = edge.start_id
        elif edge.start_id == last_vertex:
            next_vertex = edge.end_id
        else:
            raise LogicErrorError("The current last vertex in the path is not part of the given edge.")

        self._vertices.append(next_vertex)
        self._edges.append((edge.start_id, edge.end_id, edge.id))

    def pop(self):
        if not self._edges:
            raise IndexError("Path contains no relationships.")

        self._vertices.pop()
        self._edges.pop()

    def vertex_at(self, index: int) -> Vertex:
        return Vertex(self._vertices[index], self._graph)

    def edge_at(self, index: int) -> Edge:
        return Edge(self._edges[index], self._graph)

    def size(self) -> int:
        return len(self._edges)
