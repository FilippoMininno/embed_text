![Build Status](https://img.shields.io/github/actions/workflow/status/FilippoMininno/read_pdf/MainDistributionPipeline.yml)

# embed_text — DuckDB Extension

A DuckDB extension that adds an `embed_text()` scalar function. It calls any OpenAI-compatible `/v1/embeddings` HTTP endpoint and returns the result as a `FLOAT` array, making it easy to generate embeddings directly inside SQL queries.

Compatible with: **Ollama**, **llama.cpp server**, **LMStudio**, **vLLM**, **LocalAI**, and the real **OpenAI API**.

---

## Building from source

### 1. Install VCPKG

```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### 2. Build

```shell
make
```

The relevant build outputs are:

```
./build/release/duckdb                                   # DuckDB shell with the extension loaded
./build/release/test/unittest                            # test runner
./build/release/extension/embed_text/embed_text.duckdb_extension  # loadable binary
```

---

## Configuration

Before calling `embed_text()`, set the three session parameters and optionally a secret for authenticated APIs.

### Session settings

| Setting | Default | Description |
|---|---|---|
| `embed_text_endpoint` | `http://localhost:11434/v1/embeddings` | Full URL of the embeddings endpoint |
| `embed_text_model` | *(required)* | Model name sent in the request body |
| `embed_text_dimensions` | *(required)* | Size of the embedding vector returned by the model |
| `embed_text_max_batch_rows` | `512` | Maximum number of texts per HTTP request (must be ≤ 2048) |
| `embed_text_max_batch_bytes` | `1048576` | Maximum total byte size of texts per HTTP request |

```sql
SET embed_text_endpoint       = 'http://localhost:11434/v1/embeddings';
SET embed_text_model          = 'nomic-embed-text';
SET embed_text_dimensions     = 768;
SET embed_text_max_batch_rows = 512;    -- optional, default 512
SET embed_text_max_batch_bytes = 1048576; -- optional, default 1 MiB
```

### Authentication (optional)

For APIs that require an API key, create a secret:

```sql
-- No authentication (local models)
CREATE SECRET (TYPE embed_text, api_key '');

-- Authenticated API (e.g. OpenAI)
CREATE SECRET (TYPE embed_text, api_key 'sk-...');
```

The key is sent as `Authorization: Bearer <api_key>` and is never logged or exposed.

---

## Usage

### Basic embedding

```sql
LOAD 'embed_text.duckdb_extension';

SET embed_text_endpoint  = 'http://localhost:11434/v1/embeddings';
SET embed_text_model     = 'embeddinggemma';
SET embed_text_dimensions = 768;

CREATE SECRET (TYPE embed_text, api_key '');

-- Returns FLOAT[768]
SELECT embed_text('hello world');
```

### Embedding a table column

```sql
CREATE TABLE documents (id INT, content VARCHAR);
INSERT INTO documents VALUES (1, 'DuckDB is fast'), (2, 'embeddings are useful');

SELECT id, embed_text(content) AS vec FROM documents;
```

### Embedding a CTE

```sql
WITH movies(id, title) AS (
    VALUES
        (1, 'Inception'),
        (2, 'The Matrix'),
        (3, 'Interstellar'),
        (4, 'Blade Runner 2049'),
        (5, 'Dune')
)
SELECT
    m.id,
    m.title,
    embed_text(m.title) AS vec
FROM movies AS m
ORDER BY m.id;
```

All five titles are sent in a single HTTP request (one batch). NULL and empty-string inputs are skipped and return NULL in the result.

---

## Running the tests

```shell
make test
```

## Roadmap 

- [ ] **Per-batch failure isolation.** When a sub-batch request returns HTTP 400
  (e.g. one oversized or malformed input poisoning the whole array), retry that
  sub-batch as individual single-input requests to isolate the offending row.
  Only that row's output is marked NULL; the rest of the batch still succeeds.
  Keeps batch throughput on the happy path and pays the per-row cost only when
  something is actually wrong.

- [ ] **Intra-batch deduplication.** For low-cardinality columns (repeated
  titles, genres, etc.), deduplicate identical input strings within a sub-batch,
  send only the uniques, and fan the returned embeddings back out to every
  matching row. Reduces tokens billed / compute on the backend with no change to
  results.
