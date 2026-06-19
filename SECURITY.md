# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 5.x     | ✅        |
| < 5.0 (upstream libev 4.x) | ❌ (report upstream) |

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Instead, open a private report via GitHub Security Advisories:

> Repository → **Security** tab → **Report a vulnerability**

(or contact the maintainers privately through the [isndev](https://github.com/isndev)
organization).

Please include:

- a description of the vulnerability and its impact,
- the affected version / commit and platform/backend (epoll, kqueue, io_uring,
  wepoll, …),
- a minimal reproducer if possible.

We aim to acknowledge reports within a few business days and will coordinate a fix
and disclosure timeline with you. As an event-loop library, areas of particular
interest are: file-descriptor lifetime/double-close, ring/buffer parsing
(io_uring, inotify, wepoll/IOCP), integer overflow in allocation sizing, and
signal/child handling.
