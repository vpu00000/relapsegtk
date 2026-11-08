# Signed AppImage releases

RelapseGtk AppImages check the configured GitHub repository for updates. Updates
are installed only after `gpgv` verifies their detached signature. Each fork uses
its own repository and signing key without updater source changes.

## Configure the fork

Create the persistent, gitignored maintainer configuration:

```sh
cp packaging/publish.env.example packaging/publish.env
```

Edit `packaging/publish.env`:

```sh
GNUPGHOME=/path/to/your/gpghome
GITHUB_REPOSITORY=your-name/your-fork
SIGNING_KEY=relapsegtksigkey
```

`GNUPGHOME` is the GPG home where the release key is created and stored.
`SIGNING_KEY` is the key's User ID. Optionally set `RELEASE_TAG`; otherwise,
packaging requires an exact tag on `HEAD`.

## Create the update key

Run these commands once from the repository root. They create an Ed25519
signing-only key valid for two years in the configured GPG home and export its
public key there. They stop without changing anything if a secret key with the
configured User ID already exists.

```sh
(
  set -euo pipefail
  set -a
  source packaging/publish.env
  set +a
  export GNUPGHOME
  : "${GNUPGHOME:?Set GNUPGHOME in packaging/publish.env}"
  key="${SIGNING_KEY:-relapsegtksigkey}"
  install -d -m 700 "$GNUPGHOME"
  if gpg --list-secret-keys "$key" >/dev/null 2>&1; then
    echo "A secret key matching $key already exists; nothing was changed" >&2
    exit 1
  fi
  gpg --quick-generate-key "$key" ed25519 sign 2y
  gpg --export "$key" > "$GNUPGHOME/relapsegtk-release.gpg"
)
```

Keep the private keyring private and back it up securely. Only the exported
public key is embedded in the AppImage.

## Build and sign

Tag the release commit, then run:

```sh
packaging/create-signed-appimage.sh
```

The script configures a Release build with updater support, packages the
AppImage, signs it, and verifies the signature. It does not publish anything.
For a local build without a tag on `HEAD`, set `RELEASE_TAG` in `publish.env`.

Outputs:

```text
Build/Release/RelapseGtk-x86_64.AppImage
Build/Release/RelapseGtk-x86_64.AppImage.sig
```

Run and test the AppImage before publishing it.

## Publish

Publishing is a separate, explicit action:

```sh
gh release create v1.4.0 \
  --repo your-name/your-fork \
  --generate-notes \
  Build/Release/RelapseGtk-x86_64.AppImage \
  Build/Release/RelapseGtk-x86_64.AppImage.sig
```

Upload exactly the AppImage and its detached signature to the same release.
