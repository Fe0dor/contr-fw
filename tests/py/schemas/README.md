# Схемы SDK платформы

`interlock-table.schema.json` — копия опубликованной схемы файла данных таблицы блокировок
из репозитория платформы (`checker/src/testdut/sdk/schemas/interlock-table.schema.json`,
PLG-024). Тесты генератора проверяют по ней `gen/interlock-table.json` (FW-241).
Схема объявлена неизменяемой на стороне платформы; при её обновлении копию заменить целиком.
