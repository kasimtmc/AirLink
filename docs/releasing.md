# Releasing

Releases are built by the [Release workflow](../.github/workflows/release.yml) on GitHub Actions.
A version tag makes a draft pre-release with every file; I check it and publish it by hand.

## Files

| Job | Runs on | Makes |
|---|---|---|
| `linux` | Ubuntu 24.04 | `AirLink-<v>-linux-x86_64.tar.gz`, `airlink_<v>_amd64.deb`, `AirLink-<v>-x86_64.AppImage` |
| `flatpak` | freedesktop 25.08 container | `AirLink-<v>-x86_64.flatpak`, repacked from the tarball |
| `windows` | Arch Linux container (MinGW GCC 14+) | `AirLink-<v>-windows-x64.zip` |
| `windows-installer` | Ubuntu 24.04 (NSIS) | `AirLink-<v>-windows-x64-setup.exe` |
| `android` | Ubuntu 24.04 | `AirLink-<v>-android-arm64.apk` |
| `release` | Ubuntu 24.04 | the draft release with the files and `SHA256SUMS` |

The Linux build is made on Ubuntu 24.04 rather than a newer distribution, so it runs on glibc
2.39 and newer. FreeType, HarfBuzz, zlib and libpng are linked in statically; the AppImage
bundles the other libraries except the sound server's, D-Bus', Vulkan's and the display
server's, which come from the system.

## Steps

1. Put the new version in `VERSION` and `Cargo.toml` (`[workspace.package] version`). Android's
   `versionCode` follows from it (0.2.0 → 200).
2. Put the Kompoz commit to build with in `KOMPOZ_REF` (it must be pushed to kasimtmc/kompoz).
3. Write the release notes in `docs/releases/v<version>.md`.
4. Commit, then tag and push the tag:

   ```bash
   git tag v0.2.0 && git push origin v0.2.0
   ```

5. When the workflow is done, open the draft on the Releases page, check the files and publish it.

Running the workflow by hand (Actions → Release → Run workflow) builds the files as artifacts
without a release.

## Android signing

The APK is signed with the key in the repository's secrets; without them the workflow makes an
unsigned APK (and warns). Once, create the key and keep it safe: updates must be signed with the
same key, or Android refuses them.

```bash
keytool -genkeypair -v -keystore airlink-release.jks -alias airlink -keyalg RSA -keysize 4096 -validity 10000
```

Then add the secrets in GitHub (Settings → Secrets and variables → Actions):

- `ANDROID_KEYSTORE_BASE64`: the output of `base64 -w0 airlink-release.jks`
- `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` (`airlink`), `ANDROID_KEY_PASSWORD`

The same variables without `ANDROID_` and with `AIRLINK_` (`AIRLINK_KEYSTORE` is the .jks path)
sign a local `./gradlew assembleRelease`.

A release-signed APK can't update the debug builds installed with `installDebug` (another key):
uninstall the debug build first. Pairings live in the app's data, so pair the devices again.

## Building locally

The same scripts run outside CI, in `scripts/release/`:

- `linux-deps.sh`, `build-linux.sh`, `package-linux.sh`: on Ubuntu 24.04 (or in an
  `ubuntu:24.04` container) for a build other distributions can run.
- `build-windows.sh`: with MinGW-w64 GCC 14 or newer; `windows-installer.sh` with NSIS.

Kompoz must be at `../kompoz` with `scripts/fetch-deps.sh` run. The files go to `dist/`.
