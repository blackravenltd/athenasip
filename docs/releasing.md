# AthenaSIP - Releasing

A release is a tag on `main` and what is built from it: the Docker image, a Debian package
for amd64 and arm64, and the Homebrew formula's source. Everything is built from the tag, so
what users install is exactly what the tag says.

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
mkdir -p dist

# The Debian package for amd64, in Docker.
git archive "$VERSION" | docker build --target deb --output dist -

# The Debian package for arm64, natively on corvus-fi-1 (an arm64 Debian 13 host with Boost
# built statically in /opt/athenasip/deps). Docker on an Intel Mac would emulate it for hours.
git archive --prefix="athenasip-$VERSION/" "$VERSION" | ssh root@10.35.1.20 "rm -rf /tmp/athenasip-release && mkdir -p /tmp/athenasip-release && tar -x -C /tmp/athenasip-release"
ssh root@10.35.1.20 "cd /tmp/athenasip-release/athenasip-$VERSION && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBOOST_ROOT=/opt/athenasip/deps -DBoost_USE_STATIC_LIBS=ON && nice -n 15 cmake --build build -j3 && cd build && cpack -G DEB"
scp "root@10.35.1.20:/tmp/athenasip-release/athenasip-$VERSION/build/athenasip_${VERSION}_arm64.deb" dist/

# The Docker image, tagged with the version and latest.
git archive "$VERSION" | docker buildx build --platform linux/amd64 \
  -t "tomcully/athenasip:$VERSION" -t tomcully/athenasip:latest --push -
```

Check each package installs before it is published:

```bash
docker run --rm -v "$PWD/dist:/dist:ro" debian:trixie-slim sh -c \
  "apt-get update -qq && apt-get install -y -qq /dist/athenasip_${VERSION}_amd64.deb && athenasip --version"
```

## 3. Publish the release on GitHub

```bash
gh release create "$VERSION" dist/athenasip_${VERSION}_amd64.deb dist/athenasip_${VERSION}_arm64.deb \
  --verify-tag --title "AthenaSIP $VERSION" --notes "<what it is for; the entry in TODO/COMPLETED.md>"
```

Or on the website: **Releases**, **Draft a new release**, choose the tag, give it a title
and notes, drag the two `.deb` files into the assets box, **Publish release**.

GitHub adds the source archives itself. Users then install a package with:

```bash
wget https://github.com/blackravenltd/athenasip/releases/download/$VERSION/athenasip_${VERSION}_amd64.deb
sudo apt install ./athenasip_${VERSION}_amd64.deb
```

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
