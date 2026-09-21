# Shrek 2: The Game (PC) — Android Port via recomp-kit

Порт ПК-версии игры **«Шрек 2» (Shrek 2: The Game / Team Action, 2004)** на **Android** с помощью инструмента статической рекомпиляции **recomp-kit**.

---

## 📖 Как это работает

`recomp-kit` — это инструмент **статической рекомпиляции (Static Recompiler)**:
- Игра **НЕ эмулируется** (нет Box64, FEX, Wine или виртуальных машин x86).
- Исполняемый 32-битный x86 PE-код игры (`Game.exe` и вспомогательные DLL) декомпилируется через **Ghidra**, после чего транслятор переводит машинные инструкции x86 непосредственно в **нативный C-код**.
- C-код компилируется компилятором Clang под целевую архитектуру Android (`arm64-v8a`) с помощью Android NDK.
- Графика (DirectX), звук (DirectSound / Miles Sound System) и ввод перенаправляются через встроенные нативные адаптеры в **SDL3** и **Vulkan**.
- Результат — полноценное нативное Android-приложение (`.apk`) с поддержкой сенсорного геймпада и аппаратных контроллеров.

---

## 📁 Необходимые файлы игры

ПК-версия Shrek 2 построена на движке **Unreal Engine 2** (кастомный билд KnowWonder). Для портирования необходима установленная ПК-версия игры со следующей структурой:

```text
games/shrek2/original/
├── System/
│   ├── Game.exe           <- Главный исполняемый файл (ОБЯЗАТЕЛЬНО No-CD / DRM-free!)
│   ├── Core.dll
│   ├── Engine.dll
│   ├── Window.dll
│   ├── D3DDrv.dll
│   ├── DefOpenAL.dll
│   ├── Fire.dll
│   ├── KWGame.dll
│   ├── SHGame.dll
│   ├── *.u                <- UnrealScript пакеты
│   ├── *.ini, *.int
├── Animations/            <- Модели и анимации (.ukx)
├── Maps/                  <- Уровни (.unr)
├── Music/                 <- Музыкальные треки (.ogg)
├── Sounds/                <- Звуковые эффекты (.uax)
├── StaticMeshes/          <- Статические меши (.usx)
├── Textures/              <- Текстуры (.utx)
└── KarmaData/             <- Физика (.ka)
```

> ⚠️ **КРИТИЧЕСКИ ВАЖНО: Защита от копирования (DRM / SafeDisc / SecuROM)**:  
> Оригинальные дисковые версии 2004 года защищены SafeDisc / SecuROM. Статический рекомпилятор **не может** разобрать зашифрованный или упакованный исполняемый файл.  
> **Используйте распакованный / отученный от диска No-CD `Game.exe`!**

---

## 🚀 One-Click GitHub Actions Workflow (Сборка в 1 клик)

В репозитории настроен автоматический Workflow:  
📂 `.github/workflows/port-shrek2-android.yml`

### Как запустить в 1 клик:
1. Перейдите во вкладку **Actions** в вашем репозитории на GitHub.
2. В левой колонке выберите **«Port Shrek 2 to Android»**.
3. Нажмите кнопку **«Run workflow»** (справа вверху).
4. Выберите параметры:
   - **`game_files_url`**: (Опционально) Прямая ссылка на скачивание ZIP-архива с файлами игры (если вы не загружали их напрямую в репозиторий).
   - **`build_mode`**: 
     - `full` — полный цикл: распаковка, Ghidra-анализ, трансляция x86 в C, компиляция NDK и сборка APK.
     - `stub` — быстрый тестовый запуск сборки Android APK (без файлов игры, для проверки toolchain).
   - **`skip_ghidra`**: Поставьте `true`, если вы уже экспортировали ассемблерные листинги в `games/shrek2/analysis`.
5. Нажмите зелёную кнопку **«Run workflow»**.
6. По завершении скачайте готовый артефакт **`Shrek2Recomp-Android-debug-apk`**.

---

## 🛠️ Сборка локально (на компьютере)

### 1. Подготовка окружения
- **Python**: 3.11+
- **JDK**: OpenJDK 21 (необходим для Ghidra 12.1.3 и Android Gradle)
- **Android SDK & NDK**: NDK версии `27.2.12479018`, SDK Platform 36, Build-Tools 37.0.0
- **Ghidra**: [Ghidra 12.1.3](https://github.com/NationalSecurityAgency/ghidra/releases)
- **CMake & Ninja**: `pip install cmake==4.1.2 ninja==1.13.0`

Установка Python-зависимостей:
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-dev.txt
```

### 2. Подготовка и верификация файлов игры
Поместите папку с игрой в `games/shrek2/original/` и запустите авто-конфигуратор:
```bash
python3 tools/prepare_shrek2.py --game-dir games/shrek2 --install games/shrek2/original
```
Скрипт автоматически:
- Найдёт `Game.exe`.
- Проверит PE-заголовки (Image Base, Entry Point, Size of Image).
- Проверит отсутствие шифрования DRM (SafeDisc/SecuROM).
- Запишет точный SHA-256 хэш в `games/shrek2/game.toml`.
- Обнаружит модули Unreal Engine 2 в `System/`.

### 3. Экспорт ассемблерных листингов через Ghidra
```bash
python3 tools/setup.py \
  --game-dir games/shrek2 \
  --install games/shrek2/original \
  --ghidra-home /path/to/ghidra_12.1.3_PUBLIC
```
Ghidra разберёт функции и сохранит `functions.tsv` и ассемблерный код в `games/shrek2/analysis/Game.exe/functions/`.

### 4. Рекомпиляция и сборка APK
```bash
python3 tools/build.py --game-dir $(pwd)/games/shrek2 --target android --no-install --regenerate
```
Собранный APK будет находиться по пути:  
`build/android/app/build/outputs/apk/debug/app-debug.apk`

---

## 🎮 Управление на Android

Раскладка сенсорного геймпада и кнопок в `games/shrek2/game.toml` полностью настроена под официальное ПК-меню Shrek 2:

### Движение персонажа
- **FORWARD (Вперед)** — `W` (Левый стик вверх / D-Pad вверх)
- **BACKWARD (Назад)** — `S` (Левый стик вниз / D-Pad вниз)
- **STRAFE LEFT (Влево)** — `A` (Левый стик влево / D-Pad влево)
- **STRAFE RIGHT (Вправо)** — `D` (Левый стик вправо / D-Pad вправо)

### Действия и атака
- **ATTACK (Атака)** — `Left Mouse or Enter` (Левая кнопка мыши / Enter):
  - Кнопка **Крест / ✕ (Cross)** или триггеры **R1 / R2**
- **JUMP (Прыжок)** — `Right Mouse or Ctrl` (Правая кнопка мыши / Ctrl):
  - Кнопка **Квадрат / □ (Square)** или триггеры **L1 / L2**
- **SPACE (Пробел)** — `Space`:
  - Кнопки **Круг / ○ (Circle)** и **Треугольник / △ (Triangle)**
- **MOUSE LOOK (Обзор камеры)** — Свободное вращение камеры:
  - **Правый стик** (эмуляция курсора мыши с чувствительностью)

### Интерфейс и меню
- **Start** — `Esc` (Пауза и выход в меню)
- **Select** — `Tab` (Карта и статистика уровня)
- **Кнопка меню / PS** — Вызов настроек `recomp-kit` и визуального редактора наэкранных кнопок

*Примечание:* Сенсорные кнопки можно в любой момент переставить, изменить в размере или переназначить прямо во время игры на экране через меню **Edit controls** (вызывается тапом тремя пальцами или кнопкой меню). Любые внешние Bluetooth/USB-геймпады (Xbox, PlayStation, iPega) работают автоматически.

---

## 📲 Установка и запуск на Android-устройстве

1. Установите `app-debug.apk` на Android (требуется Android 10+ / arm64-v8a).
2. Загрузите ресурсы игры:
   - **Способ 1 (через встроенный лаунчер):** Упакуйте папки `System`, `Maps`, `Textures`, `Sounds`, `Music`, `Animations` в `.zip` архив и скопируйте на телефон. При первом запуске Shrek 2 откроется встроенный проводник лаунчера — выберите ZIP-архив или папку с игрой.
   - **Способ 2 (через ADB с ПК):**
     ```bash
     python3 tools/build.py --game-dir $(pwd)/games/shrek2 --target android --push-game
     ```
3. Лаунчер проверит целостность файлов, создаст штамп верификации и запустит игру!
