# Local catalogue candidates

Maintainer tools generate candidates here for review. They are not included in
Git or in the catalogue downloaded by users. Generate them with:

```sh
python3 tools/import_catalogue.py --out index
```

candidates.json and candidates/ are inputs to fetch.py and candidate_index.py.
Approved manifests belong in the separate arospkg-index repository; mkindex.py
publishes only those approved manifests. Local candidates are not proof that a
package installs or runs.
