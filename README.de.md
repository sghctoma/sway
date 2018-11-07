# sway

Der Fortschritt dieser Übersetzung kann [hier](https://github.com/swaywm/sway/issues/1318) 
eingesehen werden.

"**S**irCmpwn's **Way**land compositor" ist ein i3-kompatibler 
[Wayland](http://wayland.freedesktop.org/)-Kompositor. Lies die 
[FAQ](https://github.com/swaywm/sway/wiki#faq). Tritt dem 
[IRC-Channel](http://webchat.freenode.net/?channels=sway&uio=d4) bei (#sway in irc.freenode.net).

[![](https://sr.ht/ICd5.png)](https://sr.ht/ICd5.png)

Falls du die Entwicklung von Sway unterstützen möchtest, kannst du das auf der 
[Patreon-Seite](https://patreon.com/sircmpwn) tun, oder indem du zu
[Entwicklungsprämien](https://github.com/swaywm/sway/issues/986) 
bestimmter Features beiträgst. Jeder ist dazu eingeladen, eine Prämie in Anspruch
zu nehmen oder für gewünschte Features bereitzustellen. Patreon ist eher dafür
gedacht, Sways Wartung und das Projekt generell zu unterstützen.

## Deutscher Support

refacto(UTC+2) bietet Support im IRC (unter dem Namen azarus) und auf Github an.
ParadoxSpiral(UTC+2) bietet Support im IRC und auf Github an.

## Releasesignaturen

Neue Versionen werden mit 
[B22DA89A](http://pgp.mit.edu/pks/lookup?op=vindex&search=0x52CB6609B22DA89A) 
signiert und [auf Github](https://github.com/swaywm/sway/releases) veröffentlicht.

## Installation

### Als Paket

Sway ist in vielen Distributionen verfügbar: versuche einfach, das „sway“-Paket
zu installieren. Falls es nicht vorhanden ist, schaue dir 
[diese Wikiseite](https://github.com/swaywm/sway/wiki/Unsupported-packages) für 
distributionsspezifische Installationsinformationen an.

Wenn du Interesse hast, Sway für deine Distribution als Paket bereitzustellen, 
schaue im IRC-Channel vorbei oder schreibe eine E‑Mail an sir@cmpwn.com (nur englischsprachig).

### Kompilieren des Quellcodes

Abhängigkeiten:

* meson
* [wlc](https://github.com/Cloudef/wlc)
* wayland
* xwayland
* libinput >= 1.6.0
* libcap
* pcre
* json-c >= 0.13
* pango
* cairo
* gdk-pixbuf2 *
* pam **
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (erforderlich für man pages)
* git

_\*Nur erforderlich für swaybar, swaybg, und swaylock_

_\*\*Nur erforderlich für swaylock_

Führe diese Befehle aus:

    meson build
    ninja -C build
    sudo ninja -C build install

In Systemen ohne logind musst du `sway` das suid-Flag geben:

    sudo chmod a+s /usr/local/bin/sway

## Konfiguration

Wenn du schon i3 benutzt, kopiere einfach deine i3 Konfiguration nach
`~/.config/sway/config`. Falls nicht, kannst du die Beispielkonfiguration
benutzen. Die befindet sich normalerweise unter `/etc/sway/config`.
Um mehr Informationen über die Konfiguration zu erhalten, führe `man 5 sway` aus.

## Verwendung

Führe `sway` von einem TTY aus. Manche Displaymanager könnten funktionieren, werden aber
nicht von Sway unterstützt (gdm scheint relativ gut zu funktionieren).
