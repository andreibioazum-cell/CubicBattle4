# Firebase

Только Realtime Database, без Auth.

## Правила

Содержимое `firebase.rules.json`:

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "event": { ".read": true, ".write": true },
    "bans": { ".read": true, ".write": true },
    "users": {
      ".read": true,
      "$nick": { ".write": "!data.exists() || newData.child('pass').val() == data.child('pass').val()" }
    },
    "rooms": {
      "$room": {
        "players": { ".read": true, "$slot": { ".write": true } },
        "chat": { ".read": true, ".write": true }
      }
    }
  }
}
```

## URL

В `game/scripts/core/config.ds`:

```dimscript
string FIREBASE="https://...", ROOM="main"
```

## Структура

```
event/ 0=нет, 1=диско, 2=снег
bans/<nick> true = забанен
users/<nick>/ nick, pass, cups, candies, cls, azum, santa, level, levels, ...
rooms/main/
  players/0..3/ uid, nick, x, y, angle, hp, alive, seq, px,py,pdx,pdy,punch, sx,sy,sdx,sdy,snow, cls, level
  chat/<key>/ uid, nick, text
```

## Админ

Аккаунт `Dimasi4ek229` / `18200` — красный ник.

Команды в чате (только для админа):
- `event 0` — тоггл диско (вкл/выкл), сообщение видно всем
- `event 1` — вкл диско
- `event 2` — снег
- `ban <ник>` — забанить, игрок не зайдёт в онлайн
- `unban <ник>` — разбанить

Баны хранятся в `/bans` и в локальном `bans.dat`.

## Враг

Человечный ИИ: появляется дольше (1.6с), скорость как у игрока с вариацией, ходит не идеально ровно (воббл), бьёт быстро, иногда промахивается, уклоняется не всегда и криво.

## Прочее

Удары передаются счётчиком `punch`, снежинки — `snow`. Чат и игроки читаются раздельно.
