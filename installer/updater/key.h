/* The Ed25519 public key that signs the update manifests of TotK Recomp Multiplayer releases (docs/updater.md,
   tools/make_update.py). The updater (update.c) accepts no manifest signed by another key. The private half is kept
   by the release maintainer outside the repository and as the GitHub Actions secret TOTKMP_UPDATE_KEY.

   A test build passes its own throwaway key with -DUPDATE_PUBLIC_KEY_HEX="..." (tests/updater_test.py), so no test
   manifest is ever signed by the release key. */
#ifndef UPDATE_PUBLIC_KEY_HEX
#define UPDATE_PUBLIC_KEY_HEX "6a22ed2e111564a0f8c4792e2f7a5e37e7d4c8fb33c4fb9331522dcd650afff1"
#endif
