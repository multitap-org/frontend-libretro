Fake core sites for core_updater_test (served with file:// URLs).

Each `<site>/v1/index/test-target.cfg` is signed with a throwaway test key
(public key in core_updater_test.cpp; the secret key was never kept) using
multitap-org/cores' `index_sign`. The "core" is a text file in a stored zip.

- site1: fake_libretro.so, commit aaaa…; site2: a newer one, commit bbbb…
- bad_zip: the zip's bytes changed after the index was made (same size)
- bad_sig: the index changed after it was signed
- every index also lists a non-free core (zip missing) and a block for another target
