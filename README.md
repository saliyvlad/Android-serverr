# Android-server

> **Автор:** Салий Владислав Павлович  
> **Группа:** ИКС-432  

---

## Описание
Комплексная система сбора, передачи, хранения и визуализации телеметрических данных о покрытии сотовых сетей (LTE). Состоит из трёх основных компонентов:
1. **Android-клиент** — фоновый сервис, собирающий GPS-координаты и параметры радиосигнала (RSRP, PCI, тип сети).
2. **C++ Backend** — сервер на ZeroMQ, принимающий данные, сохраняющий их в PostgreSQL и дублирующий в JSON-лог.
3. **Десктопный GUI** — интерактивное приложение на SDL2 + Dear ImGui + ImPlot с отображением маршрута на карте OpenStreetMap, графиками сигнала и генерацией тепловых карт покрытия.

Система разработана в рамках курсового проекта (РГР) и включает полный отчёт в LaTeX по ГОСТ 7.32-2017.

---

## 🏗 Архитектура
```
[Android Telemetry Service] 
        │ (JSON over TCP:5555)
        ▼
[ZMQ Server (REP)] → [PostgreSQL (libpq)]
        │                  │
        ▼                  ▼
[JSON Log (log.json)]  [Shared Memory (mutex)]
        │
        ▼
[GUI Thread] → Tile Cache (libcurl) → OSM Servers
        │
        ├── ImPlot: Route, Real-time Graph, History
        ── Heatmap Engine: IDW Interpolation + Multithreading + PCI Filter
```

---

## Функциональные возможности
| Компонент | Возможности |
|-----------|-------------|
| **Android-клиент** | Сбор GPS (1 сек), чтение `TelephonyManager`, фильтрация зарегистрированной соты, отправка JSON через ZeroMQ REQ, оффлайн-режим (запись в файл), Foreground Service |
| **C++ Сервер** | Приём пакетов, парсинг `nlohmann::json`, адаптация метрик под тип сети (LTE/NR/GSM), параметризованные INSERT в PostgreSQL, буферизация логов (10 записей → `FlushToDisk()`), потокобезопасное обновление `data_store` |
| **База данных** | Таблица `network_logs`, автосоздание последовательности, защита от SQL-инъекций, экспорт схемы через `pg_dump` |
| **Кэш тайлов** | Фоновая загрузка OSM-тайлов, локальное сохранение в `tile_cache/`, декодирование PNG через `stb_image`, многопоточная очередь задач |
| **GUI** | Окна телеметрии, фильтров, графиков в реальном времени, загрузки `log.json`, интерактивной карты с маршрутом, панели управления heatmap |
| **Тепловая карта** | Алгоритм IDW (Inverse Distance Weighting), многопоточный расчёт (по ядрам CPU), проекция Меркатора, градиент RSRP, фильтрация по PCI, наложение поверх тайлов |
| **Отчёт** | Полная документация в LaTeX (`memoir`), автособираемое оглавление, библиография `biblatex-gost`, таблицы/графики/схемы по ГОСТ |

---

## Стек технологий
| Уровень | Технологии |
|---------|------------|
| **Языки** | C++17, Kotlin, SQL, LaTeX |
| **Сети** | ZeroMQ (REQ/REP), libcurl, TCP/IP |
| **БД** | PostgreSQL 16+, `libpq` |
| **GUI/Графика** | SDL2, Dear ImGui, ImPlot, OpenGL 3.3, GLEW |
| **Обработка данных** | `nlohmann/json`, `stb_image`, `stb_image_write`, `std::thread`, `std::mutex` |
| **Сборка** | CMake, Ninja, MinGW64/MSYS2 |
| **Документация** | XeLaTeX, `memoir`, `biblatex-gost`, `polyglossia`/`babel` |

---

## Сборка и запуск (Backend + GUI)

### 1. Клонирование и подготовка
```bash
git clone <URL_РЕПОЗИТОРИЯ>
cd Android-backend-tiles1
```

### 2. Конфигурация БД
В файле `src/database.cpp` проверьте параметры подключения:
```cpp
#define HOST "localhost"
#define PORT "5432"
#define DB_NAME "postgres"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "your_password"
```
Убедитесь, что таблица создана:
```sql
CREATE TABLE IF NOT EXISTS network_logs (
    id SERIAL PRIMARY KEY,
    latitude VARCHAR(50), longitude VARCHAR(50),
    altitude VARCHAR(50), accuracy VARCHAR(50),
    network_type VARCHAR(20), rsrp VARCHAR(50),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

### 3. Сборка через CMake
```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### 4. Копирование зависимостей и запуск
```bash
cp /mingw64/bin/*.dll ./          
cp ../log.json ./                
./main.exe
```
При запуске:
- Подключается к PostgreSQL
- Запускается фоновый поток `FetchWorker` (кэш тайлов)
- Запускается поток `RunServer` (ZMQ на порту 5555)
- Открывается окно GUI

---


## Использование GUI
| Окно | Управление |
|------|------------|
| **Current stats** | Отображает тип сети, RSRP, координаты, точность, счётчик пакетов |
| **Filters** | `Is running?` — вкл/выкл приём сервера; `Is location/network?` — флаги для GUI |
| **Signal Graph** | График RSRP за последние 100 тиков, разбивка по PCI |
| **Log Control** | Кнопка `Load log.json` — загрузка архивных данных из файла |
| **Full Signal History** | Полный график RSRP по индексу записей |
| **Movement Route** | Карта OSM, маршрут (scatter plot), наложение heatmap |
| **Heatmap Settings** | `Enable Heatmap`, радиус интерполяции (10–40 м), прозрачность, фильтр по PCI, кнопка `Generate` |

> Тепловая карта генерируется в фоновом потоке. После нажатия `Generate` подождите 5–30 сек (зависит от CPU и объёма данных).

---

##  Структура проекта
```
Android-backend-tiles1/
├── src/
│   ├── main.cpp          # Точка входа, запуск потоков
│   ├── server.cpp        # ZMQ сервер, парсинг, логирование
│   ├── database.cpp      # libpq подключение, INSERT
│   ├── gui.cpp           # ImGui/ImPlot интерфейс, отрисовка карты
│   ├── tile_cache.cpp    # Фоновая загрузка OSM тайлов
│   ├── heatmap.cpp       # IDW интерполяция, многопоточность
│   ├── mercator.h        # Проекция Web Mercator (EPSG:3857)
│   ├── data_structures.h # Глобальные структуры, мьютексы
│   └── ...
├── third_party/          # ImGui, ImPlot, nlohmann/json, stb
├── report/               # LaTeX исходники отчёта
│   ├── 0_main.tex
│   ├── 1_title.tex … 6_chap4.tex
│   └── bibl.bib
├── build/                # Скомпилированные бинарники и DLL
└── CMakeLists.txt
```