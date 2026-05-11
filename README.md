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


## Configuration

The default config file location is /etc/simple_backup/simple_backup.json.
A sample config is provided at config/simple_backup.json.

    {
        "sources": [
            "/home/user/Documents"
        ],
        "destination": "/media/user/DRIVE/Backup"
    }

sources is a list of directories to watch. destination is where files will
be copied. The destination directory is created automatically when the service
starts, provided the parent path exists (e.g. the drive is mounted).


## Running manually

    ./build/release/simple_backup --config /path/to/simple_backup.json --foreground

Options:

    --config <path>    Path to the config file
    --foreground       Log to stderr instead of syslog
    --verbose          Enable debug logging


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
