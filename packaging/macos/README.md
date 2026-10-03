# R2 releases

On the build Mac and each target Mac, create a private release configuration:

```sh
mkdir -p ~/.config/cha
cp packaging/macos/releases.conf.example ~/.config/cha/releases.conf
chmod 600 ~/.config/cha/releases.conf
```

Edit that file with the R2 bucket URL, access-key ID, and secret key. These are
the same fields as CHA's R2 storage settings. The script does not read vault
databases. Use read/write credentials on the build Mac and read-only credentials
on target Macs. The bucket stays private; packages use `packages/macos/`, away
from root-level vault objects. This requires curl 7.75 or newer (included in
supported macOS versions).

Build and publish from the repository:

```sh
make package VERSION=0.5.0
```

`make package-macos VERSION=0.5.0` still builds locally without uploading.
If an upload fails, retry without rebuilding:

```sh
./packaging/macos/release.sh upload 0.5.0 packages/CHA-macos-0.5.0.tar.gz
```

Copy `release.sh` to each target Mac once, for example as `~/bin/cha-update`,
and make it executable. No repository checkout or extra runtime is required:

```sh
mkdir -p ~/bin
cp packaging/macos/release.sh ~/bin/cha-update
chmod +x ~/bin/cha-update
```

Then install or update with:

```sh
~/bin/cha-update
```

The script downloads the latest published package, checks its SHA-256, macOS
requirement, bundle identity, version, and code signature, quits CHA, replaces
`/Applications/CHA.app`, and opens it. It asks for a sudo password if necessary.
It keeps the previous app until replacement succeeds. Vaults and configuration
are not changed. macOS can still require approval for the ad hoc signed app.

For installation into a directory you own, create it and run, for example:

```sh
mkdir -p ~/Applications
CHA_INSTALL_DIR="$HOME/Applications" ~/bin/cha-update
```

The last successful upload becomes latest, even if its version number is lower.
Packages include their checksum in the object name, so replacing latest cannot
mix the manifest from one build with the archive from another. Old packages
remain available in R2. The checksum protects against corruption; access to the
bucket's write credentials controls who can publish applications.
