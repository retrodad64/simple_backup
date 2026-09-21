# simple_backup

A simple file backup service for Linux. It watches one or more source
directories and copies any new or modified files to a destination directory.
When the service starts, or when the destination becomes available after being
absent, it performs a full sync to catch up on any missed changes.


## Building

Build a release binary:

    make release

The binary will be at build/release/simple_backup.

To build a Debian package instead:

    make package

The package will be at build/pkg/simple-backup_1.0.0_<arch>.deb.


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
A sample config is provided at config/simple_backup.json.

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
copied.


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

    sudo dpkg -i build/pkg/simple-backup_1.0.0_<arch>.deb

Edit the config file:

    sudo nano /etc/simple_backup/simple_backup.json

Enable and start the service:

    sudo systemctl enable --now simple_backup

The service will start automatically on every subsequent boot. To stop and
disable it:

    sudo systemctl disable --now simple_backup
