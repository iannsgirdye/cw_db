# cw_db — СУБД с on-disk B*+-индексом

C++20, типы `int` и `string`, иерархия «каталог данных → база → таблица», SQL-подобный язык, персистентность в файлах. Столбцы `INDEXED` обслуживаются **B*+-деревом на диске** (файл `table.idx.<col>` на столбец). Дополнительно: HTTP-сервер, терминальный клиент, string interning, access log, телеметрия.

Бинарники после сборки: `build/prog`, `build/prog-server`, `build/prog-client`, `build/test_bulk_build_guard`.

На WSL при сборке в каталоге на `/mnt/c/` возможна ошибка CMake `Operation not permitted` — собирайте в домашнем каталоге Linux и сделайте симлинк `build-wsl` (см. ниже).

---

## Соответствие заданию

### main_task.txt (обязательное)

| Требование | Реализация |
|------------|------------|
| C++14+ | C++20 (`CMakeLists.txt`) |
| Типы int, string | `Value`, `DataType::Int` / `Str` |
| Иерархия: система → БД → таблица | `Dbms` → `Database` → `Table`, каталог `--data/<db>/` |
| SQL-подобный язык | Flex/Bison: `src/sql/grammar/sql.l`, `sql.y` |
| `./prog` интерактивно, `./prog script.sql` пакетно | `src/apps/main.cpp` |
| Персистентность в ФС | `save` / `load`, `*.dat`, `*.idx.*` |
| NOT_NULL, INDEXED (уникальность, не NULL) | `Table::enforce_constraints`, тест 07 |
| Индекс B*+ на диске | `DiskBspIndex`, без дублирования строк (только `RecordId`) |
| Оптимизация WHERE по INDEXED | `executor.cpp`: `index_find`, `index_collect_range` |
| Валидация, информативные ошибки | `Parser`, `runtime_error` / stderr |
| SELECT → JSON-массив | `Value::to_json`, nlohmann `ordered_json` |
| Без аварийного завершения | try/catch в `main`, сервере, `run_query` |
| Ключевые слова без смешения регистра | лексер, тест 16 |
| `database.table`, `USE` | `TableRef`, executor |
| CREATE/DROP DATABASE, TABLE, DML, SELECT | грамматика + executor |
| Сравнения, BETWEEN [lo,hi], LIKE (regex) | executor, тесты 04–05 |
| INSERT: пропуски → NULL | `Table::materialize` |

### additional_tasks.txt (дополнительные)

| Задача | Реализация |
|--------|------------|
| String interning | `StringPool`, `Value::of_str_id` |
| Клиент–сервер (сокеты) | `prog-server` (Crow HTTP), `prog-client` |
| Access log | `AccessLog`, `--log`, тест 27 |
| Телеметрия RPS/latency/errors | `Telemetry`, stderr сервера, тест 27 |
| DEFAULT | `CREATE TABLE ... DEFAULT`, тесты 08–09 |
| AND / OR / скобки в WHERE | грамматика, тест 10 |
| SUM, COUNT, AVG | `AggregateFn`, тест 11 |

### recommended_libs.txt

| Библиотека | Использование |
|------------|----------------|
| Flex / Bison | `sql.l`, `sql.y` |
| nlohmann/json | JSON-вывод SELECT, access log |
| Crow (+ Asio) | HTTP API сервера |
| Boost `static_vector` для B*+ | **не используется** — своё on-disk дерево (`DiskBspIndex`, страницы 4 KiB) |

---

## Индекс на диске

### Файлы

Для таблицы `shop/products.dat`:

| Файл | Содержимое |
|------|------------|
| `products.dat` | Записи (формат v3): схема, порядок вставки, значения |
| `products.idx.<n>` | B*+ по столбцу с номером `n` в схеме |

Индекс не загружается целиком в RAM: `Pager` держит LRU-кэш **64 страниц** (~256 KiB на `.idx`).

### Страницы (4 KiB)

- Страница 0 — superblock (`root_page`, `first_leaf_page`, `size`).
- Лист — `(ключ, RecordId)`, цепочка `next_leaf`.
- Внутренний узел — `children` + separator-ключи.

Ключи: `value_io.h`, максимум 512 байт на ключ. Переполнение узла — по суммарному размеру записей.

### Операции

| Операция | Поведение |
|----------|-----------|
| Поиск / range | Спуск от корня, бинарный поиск в листе, скан `next_leaf` |
| INSERT / DELETE | `insert` / `erase`, B* split / borrow / merge |
| bulk_build | O(N) сборка из отсортированных ключей (миграция v1/v2) |
| SAVE | `flush` кэша pager + запись `.dat` |

`bulk_build_guard`: проверка NULL, размера ключа, оценки RAM перед bulk; при миграции v1/v2 — `assert_indexed_column_not_null` в `Table`.

### Миграции `.dat`

| Версия | Индекс при load |
|--------|-----------------|
| v1 | Нет `.idx` → `rebuild_indexes_on_disk` → `bulk_build` |
| v2 | Списки id в хвосте `.dat` → `bulk_build` |
| v3 | Открытие готовых `.idx` |

---

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

---

## Ограничения (не из ТЗ, но важно знать)

- **Таблица в RAM:** все строки `records_` / `order_` загружаются при `load` целиком; для очень больших таблиц это узкое место, не только индекс.
- **bulk_build:** входной вектор всех ключей столбца в памяти; при нехватке RAM — ошибка `bulk_build_guard` (или OOM).
- **Таблица `.dat` на Windows FS через WSL:** для сборки предпочтителен ext4 в `$HOME`.
