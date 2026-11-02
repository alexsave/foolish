# fetch.py repo listfile destdir : download listed repo paths from raw.githubusercontent with curl (files are data, never run)
import sys, os, subprocess, urllib.parse
from concurrent.futures import ThreadPoolExecutor
repo, lst, dest = sys.argv[1:4]; branch = sys.argv[4] if len(sys.argv) > 4 else "master"
paths = [l.rstrip('\n') for l in open(lst) if l.strip()]
todo = [p for p in paths if not (os.path.exists(os.path.join(dest, p)) and os.path.getsize(os.path.join(dest, p)) > 0)]
print(len(paths), 'listed', len(todo), 'to fetch', flush=True)
def get(p):
    out = os.path.join(dest, p); os.makedirs(os.path.dirname(out), exist_ok=True)
    u = f"https://raw.githubusercontent.com/{repo}/" + branch + "/" + urllib.parse.quote(p)
    r = subprocess.run(['curl', '-sfL', '-m', '300', '--retry', '3', '-o', out, u])
    if r.returncode: print('FAIL', p, flush=True)
with ThreadPoolExecutor(6) as ex: list(ex.map(get, todo))
print('done', flush=True)
