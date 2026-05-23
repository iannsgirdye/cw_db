# cw_db — СУБД с on-disk B*+-индексом

C++20, типы `int` и `string`, иерархия «каталог данных → база → таблица», SQL-подобный язык, персистентность в файлах. Столбцы `INDEXED` обслуживаются **B*+-деревом на диске** (файл `table.idx.<col>` на столбец). Дополнительно: HTTP-сервер, терминальный клиент, string interning, access log, телеметрия.

Бинарники после сборки: `build/prog`, `build/prog-server`, `build/prog-client`, `build/test_bulk_build_guard`.

На WSL при сборке в каталоге на `/mnt/c/` возможна ошибка CMake `Operation not permitted` — собирайте в домашнем каталоге Linux и сделайте симлинк `build-wsl` (см. ниже).

---

## Соответствие заданию


## Сборка и запуск

**Зависимости** (Debian/Ubuntu/WSL):

```bash
bash install_deps.sh
```

**Сборка** (в каталоге проекта):

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
```

**WSL, если `build/` на `/mnt/c/` падает:**

```bash
mkdir -p ~/cw_db-build && cd ~/cw_db-build
cmake -DCMAKE_BUILD_TYPE=Release /mnt/c/Users/1/Desktop/dev/cw_db
cmake --build . -j
ln -sfn ~/cw_db-build /mnt/c/Users/1/Desktop/dev/cw_db/build-wsl
```

**Запуск:**

```bash
./build/prog
./build/prog tests/demo.sql
./build/prog --data ./data tests/demo.sql
./build/prog-server --port 5432 --data ./data
./build/prog-client --host 127.0.0.1 --port 5432
```

| Бинарник | Назначение |
|----------|------------|
| `prog` | Локально: stdin или SQL-файл |
| `prog-server` | `POST /query`, access log, телеметрия |
| `prog-client` | SQL → HTTP → JSON |

**Тесты:**

```bash
python3 tests/run_tests.py
```

29 сценариев (включая `32 - bulk_build_guard` и `test_bulk_build_guard`).

---

## Структура проекта

```
cw_db/
├── CMakeLists.txt
├── install_deps.sh
├── README.md
├── include/
│   ├── core/              # Dbms, Value, StringPool
│   ├── storage/
│   │   ├── disk_bsp_index.h
│   │   ├── bulk_build_guard.h
│   │   ├── pager.h
│   │   ├── table.h
│   │   └── ...
│   ├── sql/
│   └── net/
├── src/
│   ├── apps/              # main, server_main, client_main
│   ├── storage/
│   │   ├── disk_bsp_index.cpp
│   │   ├── bulk_build_guard.cpp
│   │   └── ...
│   └── sql/grammar/
└── tests/
    ├── run_tests.py
    ├── test_bulk_build_guard.cpp
    └── *.sql
```

Данные во время работы: `data/<database>/<table>.dat`, `data/<database>/<table>.idx.<col>`.
