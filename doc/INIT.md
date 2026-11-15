# Init, services, accounts and the log

`init` is a small BSD-style state machine. It starts the service supervisor,
`fnusvc`, which reads one file per service from `/etc/rc.d`. The idea of
states, dependencies, restart limits and per-service logs comes from illumos
SMF, and the `requires=`/`before=` ordering from FreeBSD's `rcorder`.

## Stages

The system goes through `single`, `boot` and `multi`. `start=` in
`/etc/init.conf` picks how a normal boot goes: `single` drops to a root shell
first, `boot` runs the boot stage and then gives a root shell, `multi` goes
straight to multi user. Nothing from `multi` starts before all of `boot` is
done. Within a stage the order comes from `requires=` and `before=`. A
dependency loop is reported and broken.

The boot stage runs `fsck`, `root` (`mount -u /`), `sysctl`, `hostname`,
`mountall`, `swap`, `tmp`, `var`, `dmesg` and `firstboot`. If `fsck` needs a
human, init goes to single user.

## Service files

`/etc/rc.d/<name>` holds one `name=value` per line:

| field | meaning |
|---|---|
| `desc=` | description shown by `services` |
| `stage=` | `boot` or `multi` (default) |
| `run=` | a command that keeps running |
| `step=` | a command that runs once |
| `stop=` | used instead of SIGTERM |
| `requires=`, `before=`, `provides=` | ordering and aliases |
| `restart=` | `always`, `failure` or `never` |
| `limit=N/S` | N failures in S seconds means `maintenance` (default 5/600) |
| `user=`, `dir=`, `env=` | how to run it |
| `log=` | a file or `console`, default `/var/log/svc/<name>.log` |
| `timeout=` | seconds for a step, or for a service to stop |
| `on=`, `secure=` | the terminal it runs on, and whether root may log in |

`services=` in `/etc/rc.conf` lists the enabled services. `services enable`
and `services disable` edit it.

## States

`waiting`, `starting`, `running`, `done`, `failed`, `maintenance`,
`stopping`, `stopped`, `absent` (its terminal is missing) and `blocked`
(waiting on a service that failed or is disabled).

A service that exits is restarted according to `restart=`. Failures count
against `limit=`. Past the limit it stays in `maintenance` until
`services clear`. A service that keeps dying within five seconds waits thirty
seconds before the next start. Everything is logged with a timestamp.

Each service gets `/var/run/<name>`, owned by the user it runs as.

## Only one copy

init runs only as pid 1. The supervisor runs only as a child of init and
locks `/var/run/fnusvc.pid`. syslogd, powerd, moused and fnuplug lock their
own pid files with `pidfile(3)`, so a second copy prints `already running`
and exits.

## Shutdown

init gives the supervisor 90 seconds (`shutdown_timeout`). Services stop in
reverse order with SIGTERM to the process group, or their `stop=`, then
SIGKILL after `timeout=`. Then init sends SIGHUP, SIGTERM and SIGKILL to
whatever is left.

## Single user

The loader waits `autoboot_delay` seconds (3). Enter boots, `s` boots single
user, any other key stops the countdown. `boot_single="YES"` in
`/boot/loader.conf` does the same. The root is mounted read-only. With
`console=insecure` in `/etc/init.conf` (the default) the root password is
asked. ^D continues to multi user.

`fsck -p` checks what needs it and `mount -uw /` makes the root writable.
From multi user, `mount -urf /` makes it read-only again.

## Accounts

`/etc/master.passwd`, `/etc/passwd` and `/etc/group` ship with `root`,
`daemon` and `nobody` only. Other accounts are made with `useradd`: own
group, home in `/home` from `/etc/skel`, locked until `passwd` sets a
password. Defaults are in `/etc/users.conf`. The other tools are `usermod`,
`userdel`, `groupadd`, `groupdel`, `groupmod`, `chsh` and `chfn`. The
`firstboot` step creates `admin`.

## Logging

`/dev/klog` carries kernel messages and `syslog(3)` messages. `syslogd`
writes them as `/etc/syslog.conf` says. `login`, `su`, `doas`, `sshd`,
`passwd` and the account tools log to `auth`. `dmesg` shows the kernel
messages, or everything with `-a`.
