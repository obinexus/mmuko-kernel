# cython: boundscheck=False, wraparound=False, language_level=3
from libc.math cimport INFINITY

cpdef tuple shortest_path(dict graph, str start, str target):
    """Return (cost, path). Edge costs must be non-negative."""
    cdef dict distance = {name: INFINITY for name in graph}
    cdef dict previous = {}
    cdef set pending = set(graph)
    cdef object current, neighbour, node
    cdef double candidate, weight, best

    if start not in graph or target not in graph:
        raise KeyError("start or target is absent from the module graph")
    distance[start] = 0.0

    while pending:
        current = None
        best = INFINITY
        for node in pending:
            if distance[node] < best:
                best = distance[node]
                current = node
        if current is None:
            break
        pending.remove(current)
        if distance[current] == INFINITY or current == target:
            break
        for neighbour, weight in graph[current]:
            if weight < 0:
                raise ValueError("Dijkstra scoring requires non-negative costs")
            if neighbour not in pending:
                continue
            candidate = distance[current] + weight
            if candidate < distance[neighbour]:
                distance[neighbour] = candidate
                previous[neighbour] = current

    if distance[target] == INFINITY:
        raise ValueError(f"no load route from {start!r} to {target!r}")
    path = [target]
    current = target
    while current != start:
        current = previous[current]
        path.append(current)
    path.reverse()
    return distance[target], path
