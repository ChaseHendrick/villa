# Agent bridge tests

`smoke_offscreen.py` is the hermetic integration test used by CI. It launches
the compiled VC3D binary with Qt's offscreen platform, checks all method
descriptors, and exercises transport and headless error paths without local
volume fixtures.

Run it in the bridge container:

```sh
docker exec vc3d-bridge bash -lc 'cd /work && QT_QPA_PLATFORM=offscreen python3 \
  apps/VC3D/agent_bridge/test/smoke_offscreen.py \
  --vc3d build/ci-release-gcc/bin/VC3D'
```

After changing a method descriptor, regenerate the checked-in description with
the same command plus `--update-description-snapshot`.

`manual_bridge_test.py` is the entry point for the developer-only fixture suite
and benchmark. Its `manual_bridge_*.py` modules group checks by domain and are
not separate commands. The suite requires the local volume-package JSON files
described in `manual_bridge_support.py`:

```sh
python3 apps/VC3D/agent_bridge/test/manual_bridge_test.py offscreen
python3 apps/VC3D/agent_bridge/test/manual_bridge_test.py live
```

`manual_bridge_inklabels.py` is a separate command because it needs no local
fixtures: it builds a temporary project with flat segments and checks the
`ink_labels.*` methods end to end, including a save, a reload from disk and an
autosave restored after a hard kill:

```sh
python3 apps/VC3D/agent_bridge/test/manual_bridge_inklabels.py \
  --vc3d build/ci-release-gcc/bin/VC3D
```
