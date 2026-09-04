import json
import sys

from dynloader import DynamicLoader


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python server.py MANIFEST.json")
    loader = DynamicLoader(sys.argv[1])
    for line in sys.stdin:
        request = json.loads(line)
        try:
            response = {"ok": True, "result": loader.load(request["module"])}
        except Exception as exc:
            response = {"ok": False, "error": str(exc)}
        print(json.dumps(response), flush=True)


if __name__ == "__main__":
    main()

