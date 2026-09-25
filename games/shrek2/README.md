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

## 🚀 One-Click GitHub Actions Workflows (Сборка в 1 клик)

В репозитории настроены автоматические Workflows:  
- 📂 `.github/workflows/port-shrek2-android.yml` — Сборка порта на **Android** (`.apk`)
- 📂 `.github/workflows/port-shrek2-pc.yml` — Сборка порта на **ПК** (**Windows** `.zip` и **Linux** `.tar.gz`)

---

### Сборка версии для ПК (Windows & Linux) в 1 клик:
1. Перейдите во вкладку **Actions** в репозитории на GitHub.
2. В левой колонке выберите **«Port Shrek 2 to PC (Windows & Linux)»**.
3. Нажмите кнопку **«Run workflow»** (справа вверху).
4. Выберите параметры:
   - **`game_files_url`**: Прямая ссылка на скачивание ZIP-архива с файлами игры (если вы не загружали их напрямую в репозиторий).
   - **`target_platform`**:
     - `windows` (по умолчанию) — автономный ZIP-пакет для Windows 10/11 x64 с `Shrek2Recomp.exe`.
     - `linux` — архив для Linux x86_64 (`.tar.gz`).
     - `both` — собрать версии сразу и для Windows, и для Linux.
   - **`bundle_game_files`**: `true` — автоматически упакует папки игры (`System`, `Maps`, `Textures`, `Sounds` и др.) внутрь готового архива. В результате получится автономный релиз, готовый к запуску сразу после распаковки («распакуй и играй»).
   - **`build_mode`**: 
     - `full` — полная компиляция игры.
     - `stub` — быстрый тестовый билд для проверки работоспособности.
5. Нажмите зелёную кнопку **«Run workflow»**.
6. По завершении скачайте готовый артефакт:
   - **`Shrek2Recomp-PC-Windows`** (для Windows): распакуйте ZIP в любую папку и запустите `Play-Shrek2.bat` или `Shrek2Recomp.exe`!
   - **`Shrek2Recomp-PC-Linux`** (для Linux): распакуйте архив и запустите `./start-shrek2.sh`.

---

### Сборка версии для Android в 1 клик:
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

---

## 🚀 Поддержка Turnip Vulkan драйверов (для Snapdragon / Adreno)

**Mesa Turnip** — это открытый драйвер Vulkan для видеочипов Qualcomm Adreno (Snapdragon), который обеспечивает нативную поддержку современного **Vulkan 1.3**, динамический рендеринг (`VK_KHR_dynamic_rendering`), аппаратное сжатие текстур BCn и максимальный FPS, обходя ограничения и баги заводских драйверов.

В порт добавлена **полная поддержка загрузки Turnip из стандартных ZIP-архивов** (пакеты AdrenoTools / Mesa Turnip от K11MCH1, WinNative, Mesa-Turnip-Builder и др.):

### Способ 1: Установка ZIP-архива прямо в лаунчере на телефоне (Рекомендуется)
1. Скачайте любой совместимый ZIP-архив с Turnip драйвером на телефон (например, из релизов *Mesa-Turnip-AdrenoTools* или *WinNative*).
2. Запустите игру.
3. В лаунчере нажмите **«Manage...»** -> **«Install Vulkan driver (ZIP)...»** (если встроенный GPU не поддерживается, кнопка также появится на главном экране).
4. Выберите скачанный `.zip` файл через проводник Android.
5. Лаунчер автоматически распакует `libvulkan_freedreno.so` во внутреннее защищённое хранилище приложения, установит права на исполнение и активирует Turnip.
6. Перезапустите игру — игра запустится на драйвере Turnip!

*(Для возврата на системный драйвер Qualcomm нажмите **«Manage...»** -> **«Reset to system Vulkan driver»**).*

### Способ 2: Копирование архива в папку игры по USB
1. Подключите телефон к компьютеру или откройте файловый менеджер (например, ZArchiver).
2. Поместите архив с драйвером под именем **`turnip.zip`** или **`driver.zip`** в папку:
   `Android/data/dev.recompkit.shrek2/files/`
3. При следующем запуске игра автоматически обнаружит архив, распакует его и переключится на Turnip!

### Способ 3: Вшивание Turnip в APK при сборке через GitHub Actions
При запуске сборки в **GitHub Actions** (Workflow *Port Shrek 2 to Android*) укажите прямую ссылку на Turnip ZIP в поле **`turnip_driver_url`**. Драйвер будет автоматически скачан и интегрирован внутрь APK.
