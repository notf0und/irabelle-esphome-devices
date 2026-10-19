# Maintaining this repository

Notes for myself (and anyone else who wants to fork it) on how this repo is
kept in sync with the devices it was written for.

## Source of truth

The device packages started as files in my Home Assistant/ESPHome setup
(`smarthome/config/esphome/config/`). This repository is the **public copy**:
it is what users install, so it must stay self-contained and free of anything
personal.

```
components/                custom components (no per-user values)
packages/common/           wifi/api/ota/logger/time/uptime/captive portal
packages/devices/          hardware definitions
devices/<device>/          device package + assets + scripts + docs
examples/                  what users copy
docs/                      user-facing guides
```

## Sync workflow

1. Develop in the smarthome config as usual (that is where the device runs and
   where the iteration happens).
2. Copy the changed files into this repo. For the Waveshare device the mapping
   is:

   | smarthome path | repo path |
   |----------------|-----------|
   | `waveshare-amoled-216.yaml` | `devices/waveshare-amoled-216/waveshare-amoled-216.yaml` |
   | `waveshare-advanced-audio.yaml` | `devices/waveshare-amoled-216/advanced-audio.yaml` |
   | `apps/` | `devices/waveshare-amoled-216/apps/` |
   | `packages/common/*.yaml` | `packages/common/` |
   | `packages/devices/waveshare/` | `packages/devices/waveshare/` |
   | `custom_components/{axp2101,waveshare_duplex_audio,lvgl_screenshot,stream_microphone,sh_radio,resampler}` | `components/` |
   | `sounds/nabu/` | `devices/waveshare-amoled-216/sounds/` |
   | `partitions.csv`, `srmodels.bin`, `scripts/` | `devices/waveshare-amoled-216/` |

3. Sanitize (checklist below), then validate.
4. Commit, push, tag if the device behaviour changed.

## Sanitizing checklist

Run these before every push:

```bash
grep -rn "192\.168\|/home/\|/config/\|gonzalo\|office-\|\.local" --include="*.yaml" .
grep -rn "!secret" --include="*.yaml" .        # only the documented names may appear
```

- Never commit `secrets.yaml` (gitignored) or keys/passwords in YAML.
- No private IPs, hostnames or user paths. Per-device defaults belong in
  `substitutions:` in the device package.
- Home Assistant entity references must be derived from substitutions
  (e.g. `media_player.${friendly_name_slug}_media_player`), never hardcoded.
- `*.bak-*` files, `__pycache__` and `.esphome/` must not end up in the repo.

## Validating

**Offline (before pushing)** — point a scratch config at the local checkout:

```bash
mkdir -p /config/__repo_test__
rsync -a ~/irabelle-esphome-devices/ /config/__repo_test__/
ln -sf ../../secrets.yaml /config/__repo_test__/secrets.yaml   # adjust depth

cat > /config/repo-check.yaml <<'EOF'
substitutions: {name: repo-check, friendly_name: Repo Check}
packages:
  device: !include __repo_test__/devices/waveshare-amoled-216/waveshare-amoled-216.yaml
EOF

# external_components still points at GitHub; either push first or patch the
# copy in __repo_test__ to `type: local, path: /config/__repo_test__/components`
docker exec esphome esphome config /config/repo-check.yaml
```

Use a different `name`/file name so you do not disturb the real device's build
directory (`.esphome/build/<config-name>`).

**From GitHub (what users actually do)** — after pushing:

```yaml
# /config/repo-check.yaml
substitutions: {name: repo-check, friendly_name: Repo Check}
packages:
  device: github://notf0und/irabelle-esphome-devices/devices/waveshare-amoled-216/waveshare-amoled-216.yaml@main
```

```bash
docker exec esphome esphome config /config/repo-check.yaml
# look for "Configuration is valid!" and the two "Cloning ..." lines
```

Delete the scratch config and the test build dir afterwards.

## Releasing

Users track `@main` by default and pick up changes on their next install (git
sources refresh daily). Tag the commits you want people to be able to pin:

```bash
git tag -a v1.1.0 -m "v1.1.0"
git push origin v1.1.0
~/.local/bin/gh release create v1.1.0 --repo notf0und/irabelle-esphome-devices \
  --title v1.1.0 --notes "$(sed -n '/## v1.1.0/,/^## /p' CHANGELOG.md | head -n -1)"
```

Update `CHANGELOG.md` first. If a change requires the one-time USB flash again
(partition table or model blob), say so in bold in the release notes.

## Keeping my own device on the package

The device can consume this repo like any user's:

```yaml
# smarthome/config/esphome/config/waveshare-amoled-216.yaml (thin version)
substitutions:
  name: waveshare-amoled-216
  friendly_name: Waveshare AMOLED 2.16
  friendly_name_slug: waveshare_amoled_2_16

packages:
  device: github://notf0und/irabelle-esphome-devices/devices/waveshare-amoled-216/waveshare-amoled-216.yaml@main
```

The old local packages/components can then be removed from the smarthome
config. Because `name`/`friendly_name`/`id` and `secrets.yaml` stay the same,
the device keeps its entity ids and its Home Assistant entry — only the files
come from elsewhere.

During heavy development it is easier to point at the local checkout instead of
pushing every iteration:

```yaml
packages:
  device: !include ../../irabelle-esphome-devices/devices/waveshare-amoled-216/waveshare-amoled-216.yaml
external_components:
  - source: {type: local, path: ../../irabelle-esphome-devices/components}
    components: [axp2101, waveshare_duplex_audio, lvgl_screenshot, stream_microphone, sh_radio, resampler]
```

Switch back to `github://…@main` (or a tag) before you call it released.

## Model blob / partition table

`devices/waveshare-amoled-216/srmodels.bin` only needs regenerating when the
pinned `espressif/esp-sr` version changes:

```bash
docker exec esphome bash /config/irabelle-esphome-devices/devices/waveshare-amoled-216/scripts/pack-models.sh
```

(the script reads the esp-sr model tree from `/config/.esphome`; override with
`ESPHOME_CONFIG_DIR=...`). Commit the new blob and mention in the release notes
that users must flash it at `0xD90000` over USB.

The partition table is stable; changing it means every user has to repeat the
one-time USB flash.
