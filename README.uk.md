# sway

**Sway** ("**S**irCmpwn's **Way**land compositor") це сумісний з i3 композитор
[Wayland](http://wayland.freedesktop.org/) (**у стані розробки**).
Ознайомтесь з [ЧаПами](https://github.com/swaywm/sway/wiki).
Приєднуйтесь до [спільноти в IRC](http://webchat.freenode.net/?channels=sway&uio=d4)
(#sway на irc.freenode.net).

[![](https://sr.ht/ICd5.png)](https://sr.ht/ICd5.png)

Якщо ви хочете підтримати розробку Sway, ви можете зробити свій внесок у
[SirCmpwn'ову сторінку Patreon](https://patreon.com/sircmpwn) або до
[фонду винагород](https://github.com/swaywm/sway/issues/986) за реалізацію
певного функціоналу.
Кожен може виставити винагороду за реалізацію довільної функції
(і, відповідно, забрати її собі, виконавши це завдання);
кошти від сторінки Patreon підтримують загальну розробку та підтримку Sway.

## Підтримка українською мовою

Якщо ви хочете отримати підтримку українською мовою, можете звернутись до користувача
Hummer12007 у IRC-спільноті. Будьте терплячі, вам обов'язково допоможуть.

Наразі переклад Sway українською ще не завершено (він неповний), проте у вас є шанс долучитись,
детальніше див. [статус](https://github.com/swaywm/sway/issues/1318#issuecomment-322277382).

## Підписи випусків

Випуски підписані ключем [B22DA89A](http://pgp.mit.edu/pks/lookup?op=vindex&search=0x52CB6609B22DA89A)
та публікуються на сторінці [GitHub](https://github.com/swaywm/sway/releases).

## Встановлення

### З пакунків

Sway доступний у багатьох дистрибутивах Linux (а також у FreeBSD).
Спробуйте встановити пакунок `sway` у вашому.
Якщо він недоступний, перегляньте цю [сторінку Wiki](https://github.com/swaywm/sway/wiki/Unsupported-packages)
для інформації щодо встановлення на вашому дистрибутиві.

Якщо ви готові та зацікавлені запакувати і підтримувати Sway у вашому
дистрибутиві, будемо раді вас бачити у нашому каналі IRC. Ви також можете
спитати порад за адресою sir@cmpwn.com.

### З вихідного коду

Встановіть залежності:

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
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (required for man pages)
* git

_\*Лише для swaybar, swaybg та swaylock_

_\*\*Лише для swaylock_

Виконайте ці команди:

    meson build
    ninja -C build
    sudo ninja -C build install

На системах **з** logind, варто встановити декілька можливостей (caps)
на виконуваний файл sway:

    sudo setcap "cap_sys_ptrace,cap_sys_tty_config=eip" /usr/local/bin/sway

На системах **без** logind, необхідно встановити біт SUID на виконуваний файл sway:

    sudo chmod a+s /usr/local/bin/sway

## Налаштування

Якщо ви вже використовуєте i3, скопіюйте свій файл налаштувань
до `~/.config/sway/config`, він має запрацювати. Інакше, скопіюйте
туди файл-зразок (зазвичай знаходиться у `/etc/sway/config`), і налаштуйте під себе.

Більше інформації щодо налаштувань можете знайти, виконавши `man 5 sway`.

## Запуск

Виконайте `sway` у TTY. Деякі дисплейні менеджери (менеджери сеансу/стільниць)
можуть працювати, але офіційно не підтримуються (проте сумісніть із gdm достатньо висока).
