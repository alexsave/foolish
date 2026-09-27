#!/usr/bin/env python3
"""TestFlight from the command line, for any product in this repo.

    testflight.py status                 builds, processing, review, groups, link
    testflight.py release [BUILD] [-m TEXT]
                                         put BUILD (default: newest VALID) in the
                                         external group, set its What to Test,
                                         submit it for beta review, wait for the verdict
    testflight.py link                   the external group's public link

The product is chosen by environment, the same way the rig picks one:
    source <product>/ios/Tools/asc.env   ASC_APP_ID, ASC_EXTERNAL_GROUP, ASC_INTERNAL_GROUP

Credentials never live here. The signing client is ~/.appstoreconnect/asc.py
(override with ASC_PY), which reads ASC_KEY_ID / ASC_ISSUER / the .p8 itself.
Uploading a build is the product ship script.s job, not this file's.

What this encodes, because each was learned the hard way:
- Only ONE build per version train can be in beta review at a time; `release`
  refuses rather than expiring the one in review (owner: never swap).
- A later build of an already-approved version usually clears review in seconds.
- The en-US betaBuildLocalization row already exists for every build: PATCH it,
  POSTing a second one collides.
- POST relationships/builds answers 204 with an empty body, which is success.
- The API cannot create an app record; that is a web-UI step, once per product.
"""
import importlib.util, os, sys, time

ASC_PY = os.environ.get("ASC_PY", os.path.expanduser("~/.appstoreconnect/asc.py"))
spec = importlib.util.spec_from_file_location("asc", ASC_PY)
asc = importlib.util.module_from_spec(spec); spec.loader.exec_module(asc)


def need(name):
    v = os.environ.get(name)
    if not v:
        sys.exit(f"{name} is not set - source the product's asc.env first")
    return v


def get(path):
    st, js = asc.call("GET", path)
    if st != 200:
        sys.exit(f"GET {path} -> {st}: {js}")
    return js


def builds(app, limit=6):
    js = get(f"/v1/builds?filter[app]={app}&sort=-uploadedDate&limit={limit}"
             "&fields[builds]=version,processingState,expired,uploadedDate,preReleaseVersion,buildBetaDetail,betaAppReviewSubmission"
             "&include=preReleaseVersion,buildBetaDetail,betaAppReviewSubmission")
    inc = {(i["type"], i["id"]): i for i in js.get("included", [])}
    out = []
    for b in js["data"]:
        r = b["relationships"]
        pre = inc.get(("preReleaseVersions", (r["preReleaseVersion"]["data"] or {}).get("id")))
        det = inc.get(("buildBetaDetails", (r["buildBetaDetail"]["data"] or {}).get("id")))
        rev = inc.get(("betaAppReviewSubmissions", ((r.get("betaAppReviewSubmission") or {}).get("data") or {}).get("id")))
        out.append({
            "id": b["id"],
            "train": pre["attributes"]["version"] if pre else "?",
            "build": b["attributes"]["version"],
            "processing": b["attributes"]["processingState"],
            "expired": b["attributes"]["expired"],
            "internal": det["attributes"]["internalBuildState"] if det else "-",
            "external": det["attributes"]["externalBuildState"] if det else "-",
            "review": rev["attributes"]["betaReviewState"] if rev else "-",
        })
    return out


def link():
    g = get(f"/v1/betaGroups/{need('ASC_EXTERNAL_GROUP')}")["data"]["attributes"]
    return g.get("publicLink") if g.get("publicLinkEnabled") else None


def cmd_status():
    app = need("ASC_APP_ID")
    print(f"{'train':>6} {'build':>5}  {'processing':<10} {'internal':<24} {'external':<28} review")
    for b in builds(app):
        x = " (expired)" if b["expired"] else ""
        print(f"{b['train']:>6} {b['build']:>5}  {b['processing']:<10} {b['internal']:<24} {b['external']:<28} {b['review']}{x}")
    print(f"public link: {link() or 'off'}")


def cmd_release(args):
    app, group = need("ASC_APP_ID"), need("ASC_EXTERNAL_GROUP")
    text = None
    if "-m" in args:
        i = args.index("-m"); text = args[i + 1]; del args[i:i + 2]
    bs = builds(app, 20)
    pick = [b for b in bs if b["processing"] == "VALID" and not b["expired"]
            and (not args or b["build"] == args[0])]
    if not pick:
        sys.exit(f"no VALID build {args[0] if args else ''} yet - is it still processing?")
    b = pick[0]
    busy = [o for o in bs if o["train"] == b["train"] and o["id"] != b["id"]
            and o["review"] in ("WAITING_FOR_REVIEW", "IN_REVIEW")]
    if busy:
        sys.exit(f"{b['train']}({busy[0]['build']}) is still in beta review; Apple allows one per "
                 f"train. Wait for it (owner rule: never expire the one in review).")
    print(f"releasing {b['train']}({b['build']}) {b['id']}")
    if text:
        locs = get(f"/v1/builds/{b['id']}/betaBuildLocalizations")["data"]
        for l in locs:
            if l["attributes"]["locale"] == "en-US":
                st, _ = asc.call("PATCH", f"/v1/betaBuildLocalizations/{l['id']}",
                                 {"data": {"type": "betaBuildLocalizations", "id": l["id"],
                                           "attributes": {"whatsNew": text}}})
                print(f"  what to test: {st}")
    st, js = asc.call("POST", f"/v1/betaGroups/{group}/relationships/builds",
                      {"data": [{"type": "builds", "id": b["id"]}]})
    print(f"  added to external group: {st}")
    if st not in (204, 200):
        sys.exit(js)
    if b["review"] == "-":
        st, js = asc.call("POST", "/v1/betaAppReviewSubmissions",
                          {"data": {"type": "betaAppReviewSubmissions",
                                    "relationships": {"build": {"data": {"type": "builds", "id": b["id"]}}}}})
        print(f"  submitted for beta review: {st}")
        if st not in (200, 201):
            sys.exit(js)
    for _ in range(30):
        rev = [o for o in builds(app, 20) if o["id"] == b["id"]][0]
        print(f"  review {rev['review']}, external {rev['external']}")
        if rev["review"] in ("APPROVED", "REJECTED"):
            break
        time.sleep(20)
    print(f"public link: {link() or 'off'}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "status":
        cmd_status()
    elif a[0] == "release":
        cmd_release(a[1:])
    elif a[0] == "link":
        print(link() or "off")
    else:
        sys.exit(__doc__)
