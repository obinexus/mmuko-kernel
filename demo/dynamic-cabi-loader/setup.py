from pathlib import Path
from setuptools import Extension, setup

try:
    from Cython.Build import cythonize
except ImportError:
    cythonize = None

source = "python/dynloader/_dijkstra.pyx" if cythonize else "python/dynloader/_dijkstra.c"
extensions = [Extension("dynloader._dijkstra", [source])]
if cythonize:
    extensions = cythonize(extensions, language_level=3)
elif not Path(source).exists():
    raise RuntimeError("install Cython or build from a source distribution containing _dijkstra.c")

setup(ext_modules=extensions)
