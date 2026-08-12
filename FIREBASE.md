# Новый Firebase для Cubic Battle (онлайн без комнат)

Игра **не ставит Android/iOS SDK**. Никакого приложения в Firebase создавать не нужно.
Нужна только **Realtime Database** и её URL. Клиент ходит туда обычным HTTPS REST.

## 1. Создать проект

1. Открой [https://console.firebase.google.com](https://console.firebase.google.com)
2. **Add project** / **Создать проект**
3. Имя любое, например `cubic-battle`
4. Google Analytics можно выключить
5. Дождись создания

**Платформу (Android / iOS / Web) не добавляй.** Кнопка «Add app» не нужна.

## 2. Включить Realtime Database

1. В меню слева: **Build → Realtime Database**
2. **Create Database**
3. Регион:
   - `europe-west1` — ближе к Европе/СНГ
   - или `us-central1` — дефолт США
4. На шаге правил выбери **Start in test mode** (на 30 дней открыто).
   Потом сразу поставь правила ниже.

Это должна быть **Realtime Database**, не Firestore.

## 3. Правила

Realtime Database → вкладка **Rules** → вставь и Publish:

```json
{
  "rules": {
    "rooms": {
      "main": {
        ".read": true,
        ".write": true
      }
    }
  }
}
```

Все игроки в одной папке `rooms/main`. Клиент сам занимает первый свободный
слот из 8 (`players/0` ... `players/7`), поэтому в одном онлайне могут
играть сразу несколько человек, а не только двое.

Для отладки можно временно полностью открыть:

```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

Не оставляй так навсегда: любой, кто знает URL, сможет стереть базу.

## 4. Скопировать URL

Вкладка **Data**. Сверху адрес вида:

```
https://ИМЯ-проекта-default-rtdb.europe-west1.firebasedatabase.app
```

или

```
https://ИМЯ-проекта-default-rtdb.firebaseio.com
```

Без слэша в конце, без `.json`.

Вставь его в `game/config.ds`:

```
str FIREBASE="https://ИМЯ-проекта-default-rtdb.europe-west1.firebasedatabase.app", ROOM="main"
```

Пересобери игру.

## 5. Как это выглядит в базе

После подключения игроков в онлайне:

```
rooms/
  main/
    players/
      0/  uid, x, y, angle, hp, alive, seq
      1/  ...
      ...
      7/  ...
    bullets/
      0/  x, y, dx, dy, active, shot
      1/  ...
      ...
      7/  ...
```

Экрана ожидания больше нет: как только клиент занял слот, им можно сразу
двигаться и стрелять. Остальные игроки подтягиваются в ту же комнату по мере
подключения. Если все 8 слотов заняты, новый клиент ждёт выхода или захватывает
слот, который молчит ~5 секунд.

## 6. Частые ошибки

| Симптом | Что проверить |
|---|---|
| «Нет соединения» | URL без опечаток, https, регион совпадает с базой |
| Не видно других игроков | Оба телефона на одном URL; правила разрешают write; подождите до 0.5 секунды на синхронизацию |
| Permission denied в логах | Правила не опубликованы или путь не `rooms/main` |
| Создал Firestore | Удали, создай именно Realtime Database |
