# Bloom filter para un join TPC-H en memoria

Ejemplo educativo C++23 con proyecciones sintéticas de PART y LINEITEM; no son datos
oficiales de dbgen ni un benchmark TPC-H. Ejecuta el equivalente a:

```sql
SELECT l.*
FROM lineitem l JOIN part p ON l.l_partkey = p.p_partkey
WHERE p.p_brand = 'Brand#23';
```

## Ejecutar

Desde la raíz, sin dependencias externas para el ejemplo:

```sh
clang++ -std=c++23 -Wall -Wextra -Wpedantic -Isrc \
  examples/bloom_join_demo.cpp -o /tmp/bloom_join_demo
/tmp/bloom_join_demo
```

Con las dependencias habituales del proyecto instaladas:

```sh
cmake -S . -B build
cmake --build build --target bloom_join_demo test_bloom_filter
ctest --test-dir build -R 'BloomFilter|BloomJoin' --output-on-failure
```

## Recorrido del código

- `src/quest/execution/bloom_filter.hpp`: bits, mezcla determinista de claves,
  inserción y consulta. Un cero prueba ausencia; todos unos solo indican posibilidad.
- `tpch_bloom_join.hpp`: `AddBuildBatch` filtra PART por marca y construye el Bloom
  y una tabla exacta de multiplicidades. `FinalizeBuild` cierra la entrada.
  `ProbeBatch` descarta negativos y verifica positivos con la tabla exacta.
- `../bloom_join_demo.cpp`: genera 100 partes y 1.000 lineitems, consume batches y
  compara todas las filas de salida con el mismo join sin Bloom.
- `../../tests/test_bloom_filter.cpp`: compara además contra un nested-loop independiente
  y verifica falsos positivos, duplicados, entradas vacías y el ciclo build/probe.

La tabla guarda conteos por clave porque la consulta devuelve solo columnas de LINEITEM.
Para devolver columnas de PART, debe guardar las filas build o sus índices y materializar
cada coincidencia. PART tiene claves únicas en TPC-H, pero el helper también conserva la
multiplicidad ante entradas duplicadas.

## Conectar Parquet después

El join recibe `std::span<const Part>` y `std::span<const Lineitem>` y no guarda
referencias a esos batches. Un adaptador puede decodificar las columnas requeridas
hacia un buffer, enviar un span y reutilizar el buffer al terminar la llamada.
Enviar todos los batches de PART antes de `FinalizeBuild`; luego consumir LINEITEM.
No hacen falta estadísticas MIN/MAX ni Bloom persistidos en Parquet.

## Alcance y extensión

- Solo inner equijoin, claves enteras no nulas y ejecución de un solo escritor.
  Las claves con signo se convierten a uint64_t igual en inserción y consulta.
- La configuración usa cantidad de bits explícita (potencia de dos) y 1..16 hashes.
  Para n claves distintas y falso positivo p, estimar m = -n*ln(p)/ln(2)^2,
  redondear m a la siguiente potencia de dos y elegir k cerca de (m/n)*ln(2).
  Subdimensionar conserva exactitud pero aumenta falsos positivos.
- La derivación usa un inicio y un paso impar sobre un tamaño potencia de dos.
  No se debe modificar el filtro mientras se consulta desde otros threads.
- No se redimensiona, borra, serializa ni combina entre nodos en esta versión.
- Los errores de configuración y de fase usan Result/Status. Las asignaciones usan
  contenedores estándar, como el resto del ejemplo; no se traduce bad_alloc a Status.
- Los contadores muestran trabajo evitado; no afirman una mejora de latencia medida.
