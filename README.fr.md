# sway

"**S**irCmpwn's **Way**land compositor" est un compositeur [Wayland](http://wayland.freedesktop.org/)
compatible avec i3, **en cours de développement**.
Lisez la [FAQ](https://github.com/swaywm/sway/wiki). Rejoignez le
[canal IRC](http://webchat.freenode.net/?channels=sway&uio=d4) (#sway sur
irc.freenode.net).

[![](https://sr.ht/ICd5.png)](https://sr.ht/ICd5.png)

Si vous souhaitez soutenir le développement de Sway, vous pouvez contribuer à [ma page
Patreon](https://patreon.com/sircmpwn) ou aux [primes](https://github.com/swaywm/sway/issues/986)
pour des fonctionnalités spécifiques.
Tout le monde est invité à réclamer une prime et vous pouvez donner une prime pour n'importe quelle
fonctionnalité souhaitée. Patreon est plus utile pour supporter l'état général et la
maintenance de Sway.

## Aide en français

[abdelq](//github.com/abdelq) fournit du support en français sur IRC et Github, dans le fuseau horaire UTC-4 (EST).

## Signatures de nouvelles versions

Les nouvelles versions sont signées avec [B22DA89A](http://pgp.mit.edu/pks/lookup?op=vindex&search=0x52CB6609B22DA89A)
et publiées [sur GitHub](https://github.com/swaywm/sway/releases).

## Installation

### À partir de paquets

Sway est disponible sur plusieurs distributions. Essayez d'installer le paquet "sway" pour
la vôtre. Si ce n'est pas disponible, consultez [cette page wiki](https://github.com/swaywm/sway/wiki/Unsupported-packages)
pour de l'information sur l'installation pour vos distributions.

Si vous êtes intéressé à maintenir Sway pour votre distribution, passez par le canal
IRC ou envoyez un e-mail à sir@cmpwn.com (en anglais seulement) pour des conseils.

### Compilation depuis la source

Installez les dépendances :

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
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (requis pour les pages man)
* git

_\*Uniquement requis pour swaybar, swaybg, and swaylock_

_\*\*Uniquement requis pour swaylock_

Exécutez ces commandes :

    meson build
    ninja -C build
    sudo ninja -C build install

Sur les systèmes sans logind, vous devez suid le binaire de sway :

    sudo chmod a+s /usr/local/bin/sway

## Configuration

Si vous utilisez déjà i3, copiez votre configuration i3 à `~/.config/sway/config` et
cela va fonctionner. Sinon, copiez l'exemple de fichier de configuration à
`~/.config/sway/config`. Il se trouve généralement dans `/etc/sway/config`.
Exécutez `man 5 sway` pour l'information sur la configuration.

Mes propres dotfiles sont disponibles [ici](https://git.sr.ht/~sircmpwn/dotfiles) si
vous voulez un peu d'inspiration. Je vous recommande aussi de consulter le
[wiki](https://github.com/swaywm/sway/wiki).

## Exécution

Exécutez `sway` à partir d'un TTY. Certains gestionnaires d'affichage peuvent fonctionner,
mais ne sont pas supportés par Sway (gdm est réputé pour assez bien fonctionner).
