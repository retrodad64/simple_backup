# simple_backup

A simple file backup service for Linux. It watches one or more source
directories and copies any new or modified files to a destination directory.
When the service starts, or when the destination becomes available after being
absent, it performs a full sync to catch up on any missed changes.


## Building

Build a release binary:

    make release

The binary will be at build/release/simple_backup.

WebDAV and SFTP destinations come from the remote_files library, and the
build finds the library through pkg-config. Install libremote-files-dev, or
point PKG_CONFIG_PATH at a remote_files deploy:

    PKG_CONFIG_PATH=~/remote_files/lib/pkgconfig make release

SFTP needs a remote_files built with libssh 0.11 or newer. A remote_files built
without libssh still links, and a config naming an SFTP server then fails to
load.

A deploy outside /usr gets a run path in the binary, so you don't need
LD_LIBRARY_PATH. To build without remote_files at all, pass REMOTE_FILES=0:

    make release REMOTE_FILES=0

A config naming a WebDAV or SFTP server then fails to load.

To see which build dependencies are missing:

    ./build.sh check

The check covers g++ with C++17, make, the nlohmann/json headers, pkg-config,
and remote_files with its WebDAV and SFTP support. It honours PKG_CONFIG_PATH
and REMOTE_FILES=0 the same way the build does, and exits with status 1 when
something the build needs is missing.

To build a Debian package instead:

    make package

The package will be at dist/simple-backup_1.0.0_<arch>.deb.


## Creating the configuration

The quickest way to get a config file is to let the program ask:

    simple_backup --create-configuration

It lists the drives it can see, asks which one holds the backup, shows the
directories on that drive so you can pick one or create one, then asks for the
directories to back up. Each source is checked as you type it, and an empty
line ends the list. It prints the finished config before writing anything.

Run as an ordinary user it writes simple_backup.json to the current directory,
and you move it into place yourself. Run as root it writes straight to
/etc/simple_backup/simple_backup.json, asks before overwriting a config that is
already there, and offers to restart the service. Say no to overwriting and it
asks where to put the file instead, leaving that file owned by you rather than
by root.

A drive that is plugged in but not mounted is still offered, and the program
mounts it for you once you pick it.


## Configuration

The default config file location is /etc/simple_backup/simple_backup.json.
Sample configs are in config/: simple_backup.json backs up to an SFTP server,
simple_backup_device.json to a drive, simple_backup_webdav.json to a WebDAV
server, and simple_backup_multiple.json to a drive and a WebDAV server at once.

    {
        "sources": [
            "/home/user/Documents"
        ],
        "destination": {
            "device": { "uuid": "1A2B-3C4D", "name": "Backup Drive" },
            "path": "Backup"
        }
    }

sources is a list of directories to watch. destination is where files are
copied: a path, a drive, a WebDAV server, or an SFTP server.


## Backing up to more than one destination

destination also takes a list. Each entry is one of the forms above, and each
gets a full copy of every source:

    "destination": [
        {
            "device": { "uuid": "34C0-66F7", "name": "Samsung USB" },
            "path": ""
        },
        {
            "webdav": {
                "url": "https://localhost:493",
                "username": "alice",
                "password_file": "/etc/simple_backup/webdav.password",
                "ca_file": "/srv/webdav-chroot/etc/webdav/tls/cert.pem",
                "name": "Local WebDAV"
            },
            "path": ""
        }
    ]

Each destination has its own sync thread and comes and goes on its own. With
the drive unplugged, changes still go to the server, and plugging the drive in
starts a full sync to the drive alone. Log messages name the destination they
are about. A mistake in one entry stops the config loading, and the error says
which entry, such as "destination 2 of 2: destination.webdav needs a url".

--synchronize runs each destination in turn. A destination that is missing or
stops answering is logged and skipped, the others still run, and the exit
status is 1. config/simple_backup_multiple.json holds the example above.


## Naming the destination drive

A removable drive doesn't always land on the same path. udisks mounts it at
/media/$USER/<LABEL>, and if a directory of that name is already there it
mounts at LABEL1 instead, then LABEL2. A config that names the path is wrong
as soon as that happens.

Name the drive itself and the service finds it wherever it lands:

    "destination": {
        "device": { "uuid": "1A2B-3C4D", "name": "Backup Drive" },
        "path": "Backup"
    }

path is relative to the drive, so this backs up to <mount point>/Backup.

Find the identifier with lsblk -f, or by listing /dev/disk/by-uuid. Use one of
these four keys, and only one:

| Key | Where it comes from | What it survives |
| --- | --- | --- |
| uuid | /dev/disk/by-uuid | Any port or machine. Lost when you reformat. |
| label | /dev/disk/by-label | Reformatting, if you set the label again. Not unique. |
| partuuid | /dev/disk/by-partuuid | Reformatting the filesystem. Absent on some drives. |
| serial | /dev/disk/by-id | Reformatting. Tied to that physical drive. |

uuid is the one to use. It belongs to the filesystem, so it follows your data.
A UUID is matched without regard to case, and a label is written the ordinary
way even though udev spells the symlink "Samsung\x20USB".

name is optional and appears in log messages, so the journal reads "waiting for
Backup Drive" instead of a bare UUID.

Add "mount": false if you only want the service to use the drive once
something else has mounted it. The default is true, which mounts it through
udisks2.


## Destination layout

Each source directory is recreated by name under the destination. With the
config above, /home/user/Documents is backed up to <mount point>/Backup/Documents.

    /home/user/Documents/report.odt  ->  <mount>/Backup/Documents/report.odt
    /home/user/Pictures/2026/a.jpg   ->  <mount>/Backup/Pictures/2026/a.jpg

This keeps multiple sources separate, so each one you add gets its own
directory alongside the others. Two sources whose last path component is the
same, such as /home/alice/Documents and /home/bob/Documents, would share one
directory at the destination and overwrite each other. The service logs a
warning at startup when it sees that, and the fix is to back up the parent
directory instead.


## Using a plain path instead

destination also accepts a path, which is the right form for a fixed disk:

    "destination": "/mnt/backup"

The service creates the last component of that path but never its parents. A
path naming a drive that isn't mounted stays unavailable, so the backup waits
instead of writing to your root filesystem. It also logs a warning if the
destination turns out to be on the same filesystem as /, which usually means
the drive failed to mount and something else created the directory.


## Backing up to a WebDAV server

Name the server with a webdav object in place of device:

    "destination": {
        "webdav": {
            "url": "https://nas.home.lan/dav",
            "username": "backup",
            "password_file": "/etc/simple_backup/webdav.password",
            "ca_file": "/etc/simple_backup/webdav-ca.pem",
            "name": "NAS"
        },
        "path": "Backup/laptop"
    }

path is relative to url, so this backs up to
https://nas.home.lan/dav/Backup/laptop/Documents and so on. The service
creates path on the server if path is missing. A sample config is at
config/simple_backup_webdav.json.

| Key | Required | Meaning |
| --- | --- | --- |
| url | yes | Root of the WebDAV share. Has to start with https://. |
| username | no | User for HTTP Basic authentication. Leave out for a server without logins. |
| password_file | with username | File whose first line is the password. |
| ca_file | no | PEM certificate to trust, for a server with a self signed certificate. |
| name | no | Shown in log messages in place of the URL. |
| connect_timeout | no | Seconds to wait for a connection. 10 by default. |
| allow_insecure_http | no | Accept an http:// url. For a local test server only. |

The config takes no password key. The config file is readable by group, and
the configurator prints the config, so the password goes in a file of its own:

    sudo install -m 600 /dev/null /etc/simple_backup/webdav.password
    sudo nano /etc/simple_backup/webdav.password

A server set up with remote_files/setup_webdav serves a self signed
certificate. Copy <chroot>/etc/webdav/tls/cert.pem from the server to
/etc/simple_backup/webdav-ca.pem and name the copy in ca_file. Without the
copy, every request fails with "SSL certificate problem: self-signed
certificate".

The service treats the server the way the service treats a drive. Every 5
seconds while the server is unreachable, and every 30 seconds once the server
answers, the service creates path on the server. Each time the server comes
back, a full sync runs. A server going away, or rejecting the password, logs
the reason once, and the service waits.

A change made while the server is down misses the upload. The service notes
the miss and runs a full sync on the next check, 30 seconds at most, after the
server answers again. A sync cut short by the server going away starts over
the same way.

WebDAV keeps no permissions, so --synchronize compares the size and the
modified time instead. The server stamps the modified time when an upload
finishes. A server whose clock runs more than 2 seconds behind this machine's
makes recently changed files look out of date, and a full sync uploads those
files again.


## Backing up to an SFTP server

Name the server with an sftp object in place of device:

    "destination": {
        "sftp": {
            "host": "nas.home.lan",
            "username": "backup",
            "identity_file": "/etc/simple_backup/id_ed25519",
            "known_hosts_file": "/etc/simple_backup/known_hosts",
            "name": "NAS"
        },
        "path": "/srv/backup/laptop"
    }

This backs up to /srv/backup/laptop/Documents on the server, and so on. A path
without a leading / starts at the login directory, so "Backup/laptop" means
~/Backup/laptop for the backup user. The service creates path on the server if
path is missing. config/simple_backup.json holds this example.

| Key | Required | Meaning |
| --- | --- | --- |
| host | yes | Server name or address. |
| port | no | 22 by default. |
| username | no | Login name. Leave out to log in as the user running the service, root for the systemd unit. |
| identity_file | no | Private key file. Leave out to try the SSH agent, then ~/.ssh/id_ed25519, id_ecdsa and id_rsa of the user running the service. |
| passphrase_file | no | File whose first line unlocks an encrypted key. |
| known_hosts_file | no | known_hosts file holding the server's host key. ~/.ssh/known_hosts by default. |
| name | no | Shown in log messages in place of the host. |
| connect_timeout | no | Seconds to wait for a connection. 10 by default. |

The login is by SSH key only. The config takes no password key, and no
passphrase key, so a secret never sits in the config file. ~/.ssh/config is
never read, so the keys above are the whole configuration.

To set up a key for the service, make one without a passphrase, and install its
public half on the server for the backup user:

    sudo ssh-keygen -t ed25519 -N '' -C simple_backup -f /etc/simple_backup/id_ed25519
    sudo ssh-copy-id -i /etc/simple_backup/id_ed25519.pub backup@nas.home.lan

The service never accepts an unknown host key, so record the server's key
before the first start. Compare the fingerprint ssh-keyscan prints with the one
`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` prints on the server:

    ssh-keyscan nas.home.lan | sudo tee /etc/simple_backup/known_hosts
    ssh-keygen -lf /etc/simple_backup/known_hosts

For a server on another port, pass `-p <port>` to ssh-keyscan. An unknown or
changed host key logs the key's SHA256 fingerprint and the ssh-keyscan command
to run, and the service waits.

The service treats the server the way it treats a WebDAV server: a check every
5 seconds while the server is unreachable and every 30 seconds once it
answers, a full sync each time the server comes back, and a full sync to catch
up on changes made while the server was down. A connection that drops
reconnects at the next check.

An upload gets the source's modified time, so the server's clock plays no part
in deciding what is out of date. A file counts as up to date when the size and
the modified time, to the second, match. --synchronize keeps the mode bits and
compares the size and the mode bits, the same as for a drive.

SFTP has no status for a full disk, so on a full server each upload fails and
logs its own error. An upload cut short leaves a partial file on the server,
which has the wrong size and gets uploaded again by the next sync.


## Adding a storage type

Copier writes through the Storage interface in include/storage.hpp, and
knows nothing about where the files land. Each storage type implements the
interface:

| Class | File | Destination |
| --- | --- | --- |
| LocalStorage | src/local_storage.cpp | A path or a mounted drive |
| RemoteFileStorage | src/remote_file_storage.cpp | A WebDAV or SFTP server, through remote_files |

Paths given to a Storage are relative to the destination root and separated by
"/", such as "Documents/2026/report.odt". An operation throws StorageError on
failure. A failure every later operation will hit too, such as a server no
longer answering, throws StorageUnavailable, so a sync stops at the first
file instead of logging one failure per file.

A new type needs a Storage subclass, a key in the destination object parsed
in src/config.cpp, and a branch in StorageTracker in src/main.cpp, which
decides when the destination is available. A protocol remote_files speaks
needs only a branch in RemoteFileStorage::create().


## Running manually

    ./build/release/simple_backup --config /path/to/simple_backup.json --foreground

Options:

    --config <path>         Path to the config file
    --foreground            Log to stderr instead of syslog
    --verbose               Enable debug logging
    --synchronize           One-shot sync, then exit
    --create-configuration  Build a config file by answering questions


## Installing as a systemd service

Install the package:

    sudo apt install ./dist/simple-backup_1.0.0_<arch>.deb

Edit the config file:

    sudo nano /etc/simple_backup/simple_backup.json

Enable and start the service:

    sudo systemctl enable --now simple_backup

The service will start automatically on every subsequent boot. The unit waits
for network-online.target, so a WebDAV or SFTP destination is reachable by
the first check. To stop and
disable it:

    sudo systemctl disable --now simple_backup
