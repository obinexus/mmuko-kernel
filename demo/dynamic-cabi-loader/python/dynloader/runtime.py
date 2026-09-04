import ctypes
import hashlib
import json
from pathlib import Path

from ._dijkstra import shortest_path


class LoaderError(RuntimeError):
    pass


CTYPE = {
    "void": None,
    "bool": ctypes.c_bool,
    "int32": ctypes.c_int32,
    "uint32": ctypes.c_uint32,
    "int64": ctypes.c_int64,
    "double": ctypes.c_double,
    "cstring": ctypes.c_char_p,
    "pointer": ctypes.c_void_p,
}


class DynamicLoader:
    """Discover -> score -> load. This class owns all loader decisions."""

    def __init__(self, manifest_path: str | Path):
        self.manifest_path = Path(manifest_path).resolve()
        self.root = self.manifest_path.parent
        self.manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        self.modules = self.manifest["modules"]
        self._handles: dict[str, ctypes.CDLL] = {}

    def _inside_root(self, relative: str) -> Path:
        candidate = (self.root / relative).resolve(strict=True)
        if candidate != self.root and self.root not in candidate.parents:
            raise LoaderError(f"path escapes loader root: {relative}")
        return candidate

    @staticmethod
    def _sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()

    def _verify_file(self, spec: dict, key: str) -> Path:
        path = self._inside_root(spec[key])
        expected = spec.get(f"{key}_sha256")
        if expected and self._sha256(path) != expected.lower():
            raise LoaderError(f"SHA-256 mismatch for {path.name}")
        return path

    def _graph(self):
        graph = {"@root": []}
        for name, spec in self.modules.items():
            graph[name] = []
            if not spec.get("dependencies"):
                graph["@root"].append((name, float(spec.get("load_cost", 1))))
        for name, spec in self.modules.items():
            for dependency in spec.get("dependencies", []):
                if dependency not in self.modules:
                    raise LoaderError(f"unknown dependency {dependency!r} for {name!r}")
                graph[dependency].append((name, float(spec.get("load_cost", 1))))
        return graph

    def _load_dll(self, name: str):
        if name in self._handles:
            return self._handles[name]
        spec = self.modules[name]
        dll_path = self._verify_file(spec, "dll")
        convention = spec.get("calling_convention", "cdecl")
        if convention == "stdcall" and hasattr(ctypes, "WinDLL"):
            handle = ctypes.WinDLL(str(dll_path))
        elif convention == "cdecl":
            handle = ctypes.CDLL(str(dll_path))
        else:
            raise LoaderError(f"unsupported calling convention: {convention}")
        for export_name, signature in spec.get("exports", {}).items():
            try:
                function = getattr(handle, export_name)
            except AttributeError as exc:
                raise LoaderError(f"missing export {export_name!r} in {dll_path.name}") from exc
            try:
                function.argtypes = [CTYPE[item] for item in signature.get("args", [])]
                function.restype = CTYPE[signature.get("returns", "void")]
            except KeyError as exc:
                raise LoaderError(f"unknown C type {exc.args[0]!r}") from exc
        self._handles[name] = handle
        return handle

    def load(self, module_name: str) -> dict:
        if module_name not in self.modules:
            raise LoaderError(f"unknown runtime module: {module_name}")
        cost, route = shortest_path(self._graph(), "@root", module_name)
        for dependency in route[1:]:
            self._load_dll(dependency)
        script = self._verify_file(self.modules[module_name], "script")
        if script.suffix.lower() not in {".js", ".mjs", ".cjs"}:
            raise LoaderError("runtime script is not JavaScript")
        return {
            "module": module_name,
            "script": str(script),
            "route": route[1:],
            "score": cost,
            "exports": sorted(self.modules[module_name].get("exports", {})),
        }

