# AthenaSIP - Releasing

A release is a tag on `main` and what is built from it: the Docker image and the Homebrew
formula's source. Everything is built from the tag, so what users install is exactly what the
tag says.

No Debian packages ship with a release for now (Tom, 2026-10-12). The Dockerfile's `deb`
target stays, so anyone can build one ([Installation](installation.md)).

## Once, before the first release

- **The GitHub CLI**, to make releases from the command line:
  `brew install gh`, then `gh auth login` (GitHub.com, HTTPS, log in with a browser).
- **Docker Hub**: `docker login -u tomcully`, for the image.
- **The Homebrew tap**: a public repository named `blackravenltd/homebrew-athenasip`. The
  `homebrew-` prefix is what makes it a tap; users name it without the prefix. Create it
  empty on GitHub, then:

  ```bash
  git clone git@github.com:blackravenltd/homebrew-athenasip.git ../homebrew-athenasip
  mkdir -p ../homebrew-athenasip/Formula
  ```

## 1. Cut the release

On `develop`, with everything for the release merged and [Testing](testing.md#before-a-release)
passing:

```bash
VERSION=0.11.0                       # the next version, x.y.z, no v

# Bump project(... VERSION ...) in CMakeLists.txt to $VERSION, then:
git commit -am "Bump to $VERSION"
git checkout main && git merge --no-ff develop -m "Merge develop: $VERSION, <what it is for>"
git tag -a "$VERSION" -m "AthenaSIP $VERSION"
git push origin main develop "$VERSION"
git checkout develop
```

## 2. Build what ships

From the tag, never the working tree:

```bash
# The Docker image, tagged with the version and latest.
git archive "$VERSION" | docker buildx build --platform linux/amd64 \
  -t "tomcully/athenasip:$VERSION" -t tomcully/athenasip:latest --push -
```

Check it reports the version before anything points at it:

```bash
docker run --rm --entrypoint athenasip "tomcully/athenasip:$VERSION" --version
```

## 3. Publish the release on GitHub

```bash
gh release create "$VERSION" --verify-tag --title "AthenaSIP $VERSION" --notes "<what it is for; the entry in TODO/COMPLETED.md>"
```

Or on the website: **Releases**, **Draft a new release**, choose the tag, give it a title
and notes, **Publish release**.

GitHub adds the source archives itself, which the Homebrew formula builds from.

## 4. Update the Homebrew formula

The formula builds from the source archive GitHub made for the tag, so it needs that
archive's address and checksum:

```bash
SHA=$(curl -sL "https://github.com/blackravenltd/athenasip/archive/refs/tags/$VERSION.tar.gz" | shasum -a 256 | cut -d' ' -f1)
sed -e "s/VERSION/$VERSION/" -e "s/SHA256/$SHA/" packaging/homebrew/athenasip.rb \
  > ../homebrew-athenasip/Formula/athenasip.rb
```

Try it before pushing; this builds it the way a user's machine will:

```bash
brew tap blackravenltd/athenasip ../homebrew-athenasip   # once; a local tap of the clone
brew install --build-from-source blackravenltd/athenasip/athenasip
brew test athenasip
brew audit --strict athenasip
```

Then publish it:

```bash
cd ../homebrew-athenasip && git add Formula/athenasip.rb && git commit -m "AthenaSIP $VERSION" && git push
```

Users install with:

```bash
brew install blackravenltd/athenasip/athenasip
brew services start athenasip
```

The formula builds from source on the user's machine, which takes a few minutes. Prebuilt
bottles are a later step and need a workflow to build them; there is no hosted CI here, so
they would be built on a Mac and uploaded by hand.

## 5. Afterwards

- Deploy the tag to the nodes ([`TODO/ACTIVE.md`](../TODO/ACTIVE.md) says what runs where)
  and run `test/interop/verify.py test/interop/live.json`.
- Move the release's entry in `TODO/COMPLETED.md` under **Releases**.
