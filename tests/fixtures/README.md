# Test Fixtures

`synthetic_wt` is a fake War Thunder-like folder for scanner validation. It contains only synthetic placeholders with Dagor-style names, so it is safe to commit and must not be treated as real game data.

The app should accept it as a plausible install marker set because it has:

- `aces.exe`
- `content/`
- `content/base/res/`
- `.vromfs.bin`, `.grp`, `.dxp.bin`, `.blk`, `riDesc.bin`, and `dynModelDesc.bin` placeholder files

The `.vromfs.bin` and `.dxp.bin` files are tiny hand-made binary fixtures used to exercise reader code. They are not copied from War Thunder. The DXP fixture includes a synthetic DDSx header so tests can validate texture metadata parsing without using game data. The BLK fixture references the synthetic GRP and DXP names so relationship matching can be tested without real game metadata.
