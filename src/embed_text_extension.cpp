#define DUCKDB_EXTENSION_MAIN

#include "embed_text_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace duckdb {

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

// Splits "http://host:port/some/path" into "http://host:port" and "/some/path".
static void ParseEndpointURL(const string &url, string &host_url, string &path) {
	auto scheme_end = url.find("://");
	if (scheme_end == string::npos) {
		throw IOException("embed_text: endpoint must start with http:// or https://: %s", url);
	}
	auto path_start = url.find('/', scheme_end + 3);
	if (path_start == string::npos) {
		host_url = url;
		path = "/";
	} else {
		host_url = url.substr(0, path_start);
		path = url.substr(path_start);
	}
}

// ---------------------------------------------------------------------------
// Config resolution (once per EmbedTextExecute call)
// ---------------------------------------------------------------------------

struct EmbedConfig {
	string host_url;
	string path;
	string model;
	string api_key;
};

static EmbedConfig ResolveConfig(ClientContext &context) {
	Value endpoint_val, model_val;
	context.TryGetCurrentSetting("embed_text_endpoint", endpoint_val);
	context.TryGetCurrentSetting("embed_text_model", model_val);

	if (model_val.IsNull() || model_val.ToString().empty()) {
		throw InvalidInputException("embed_text: SET embed_text_model = '<model>' before calling embed_text()");
	}

	EmbedConfig cfg;
	cfg.model = model_val.ToString();
	ParseEndpointURL(endpoint_val.ToString(), cfg.host_url, cfg.path);

	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_match = secret_manager.LookupSecret(transaction, "", "embed_text");
	if (secret_match.HasMatch()) {
		const auto &kv = dynamic_cast<const KeyValueSecret &>(secret_match.GetSecret());
		Value key_val;
		if (kv.TryGetValue("api_key", key_val) && !key_val.IsNull()) {
			cfg.api_key = key_val.ToString();
		}
	}

	return cfg;
}

// ---------------------------------------------------------------------------
// Batch HTTP call
// ---------------------------------------------------------------------------

// inputs[i] is the text for local batch position i.
// row_of[i] is the output row index that inputs[i] belongs to.
// On return, embeddings[row_of[i]] is populated with the float vector.
static void CallEmbeddingAPIBatch(const EmbedConfig &cfg, httplib::Client &cli, const vector<string> &inputs,
                                  const vector<idx_t> &row_of, vector<vector<float>> &embeddings) {
	nlohmann::json body;
	body["model"] = cfg.model;
	body["input"] = inputs;
	string body_str = body.dump();

	httplib::Headers headers;
	if (!cfg.api_key.empty()) {
		headers.emplace("Authorization", "Bearer " + cfg.api_key);
	}

	auto res = cli.Post(cfg.path.c_str(), headers, body_str, "application/json");
	if (!res) {
		throw IOException("embed_text: connection to '%s%s' failed: %s", cfg.host_url, cfg.path,
		                  httplib::to_string(res.error()));
	}
	if (res->status != 200) {
		throw IOException("embed_text: endpoint returned HTTP %d: %s", res->status, res->body);
	}

	nlohmann::json resp;
	try {
		resp = nlohmann::json::parse(res->body);
	} catch (const nlohmann::json::exception &e) {
		throw IOException("embed_text: failed to parse response JSON: %s (body: %s)", e.what(), res->body);
	}

	if (!resp.contains("data") || resp["data"].empty()) {
		throw InvalidInputException(
		    "embed_text: response missing 'data' array (expected OpenAI-compatible /v1/embeddings format)");
	}
	if (resp["data"].size() != inputs.size()) {
		throw InvalidInputException("embed_text: sent %zu inputs but got %zu results back", inputs.size(),
		                            resp["data"].size());
	}

	for (const auto &item : resp["data"]) {
		idx_t idx = item["index"].get<idx_t>();
		if (idx >= inputs.size()) {
			throw InvalidInputException("embed_text: response item index %llu out of range (batch size %zu)",
			                            (unsigned long long)idx, inputs.size());
		}
		vector<float> vec;
		vec.reserve(item["embedding"].size());
		for (const auto &v : item["embedding"]) {
			vec.push_back(v.get<float>());
		}
		embeddings[row_of[idx]] = std::move(vec);
	}
}

// ---------------------------------------------------------------------------
// Bind callback
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> BindEmbedText(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &arguments) {
	Value dims_val;
	context.TryGetCurrentSetting("embed_text_dimensions", dims_val);
	auto dims_signed = dims_val.IsNull() ? 0 : dims_val.GetValue<int32_t>();
	if (dims_signed <= 0) {
		throw InvalidInputException("embed_text: SET embed_text_dimensions = N before calling embed_text()");
	}
	bound_function.return_type = LogicalType::ARRAY(LogicalType::FLOAT, (idx_t)dims_signed);
	return nullptr;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

static void EmbedTextExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto dims = (idx_t)ArrayType::GetSize(result.GetType());

	Value max_rows_val, max_bytes_val;
	context.TryGetCurrentSetting("embed_text_max_batch_rows", max_rows_val);
	context.TryGetCurrentSetting("embed_text_max_batch_bytes", max_bytes_val);
	idx_t max_batch_rows = (idx_t)max_rows_val.GetValue<int32_t>();
	idx_t max_batch_bytes = (idx_t)max_bytes_val.GetValue<int32_t>();

	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto &child_vec = ArrayVector::GetEntry(result);
	auto child_data = FlatVector::GetData<float>(child_vec);
	auto &result_validity = FlatVector::Validity(result);

	UnifiedVectorFormat text_fmt;
	args.data[0].ToUnifiedFormat(count, text_fmt);
	auto text_data = (string_t *)text_fmt.data;

	auto cfg = ResolveConfig(context);
	httplib::Client cli(cfg.host_url.c_str());
	cli.set_connection_timeout(30);
	cli.set_read_timeout(60);

	// embeddings[i] is populated for each valid output row i after all batches flush.
	vector<vector<float>> embeddings(count);

	vector<string> batch_inputs;
	vector<idx_t> batch_row_of;
	idx_t batch_bytes = 0;

	auto flush = [&]() {
		if (batch_inputs.empty()) {
			return;
		}
		CallEmbeddingAPIBatch(cfg, cli, batch_inputs, batch_row_of, embeddings);
		batch_inputs.clear();
		batch_row_of.clear();
		batch_bytes = 0;
	};

	for (idx_t i = 0; i < count; i++) {
		auto src_idx = text_fmt.sel->get_index(i);
		if (!text_fmt.validity.RowIsValid(src_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}
		string text = text_data[src_idx].GetString();
		if (text.empty()) {
			result_validity.SetInvalid(i);
			continue;
		}

		if (!batch_inputs.empty() &&
		    (batch_inputs.size() >= max_batch_rows || batch_bytes + text.size() > max_batch_bytes)) {
			flush();
		}

		batch_row_of.push_back(i);
		batch_bytes += text.size();
		batch_inputs.push_back(std::move(text));
	}
	flush();

	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(i)) {
			continue;
		}
		auto &emb = embeddings[i];
		if ((idx_t)emb.size() != dims) {
			throw InvalidInputException("embed_text: expected %llu dimensions but got %zu", (unsigned long long)dims,
			                            emb.size());
		}
		for (idx_t j = 0; j < dims; j++) {
			child_data[i * dims + j] = emb[j];
		}
	}
}

// ---------------------------------------------------------------------------
// Secret type: embed_text { api_key VARCHAR }
// ---------------------------------------------------------------------------

static unique_ptr<BaseSecret> CreateEmbedTextSecret(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->TrySetValue("api_key", input);
	if (secret->secret_map.find("api_key") == secret->secret_map.end()) {
		secret->secret_map["api_key"] = Value("");
	}
	secret->redact_keys.insert("api_key");
	return std::move(secret);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	config.AddExtensionOption(
	    "embed_text_endpoint",
	    "Full URL of the embeddings endpoint, including path (e.g. http://localhost:11434/v1/embeddings)",
	    LogicalType::VARCHAR, Value("http://localhost:11434/v1/embeddings"));
	config.AddExtensionOption("embed_text_model", "Model name sent in the embeddings request body",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("embed_text_dimensions", "Embedding vector size; must be set before calling embed_text()",
	                          LogicalType::INTEGER, Value::INTEGER(0));
	config.AddExtensionOption("embed_text_max_batch_rows", "Maximum number of texts per HTTP request (must be <= 2048)",
	                          LogicalType::INTEGER, Value::INTEGER(512));
	config.AddExtensionOption("embed_text_max_batch_bytes", "Maximum total byte size of texts per HTTP request",
	                          LogicalType::INTEGER, Value::INTEGER(1048576));

	// Register secret type
	SecretType secret_type;
	secret_type.name = "embed_text";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	// Register secret creation function (provider = "config")
	CreateSecretFunction create_secret_func;
	create_secret_func.secret_type = "embed_text";
	create_secret_func.provider = "config";
	create_secret_func.function = CreateEmbedTextSecret;
	create_secret_func.named_parameters["api_key"] = LogicalType::VARCHAR;
	loader.RegisterFunction(create_secret_func);

	ScalarFunction func("embed_text", {LogicalType::VARCHAR}, LogicalType::FLOAT, EmbedTextExecute, BindEmbedText);
	loader.RegisterFunction(func);
}

void EmbedTextExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string EmbedTextExtension::Name() {
	return "embed_text";
}

std::string EmbedTextExtension::Version() const {
#ifdef EXT_VERSION_EMBED_TEXT
	return EXT_VERSION_EMBED_TEXT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(embed_text, loader) {
	duckdb::LoadInternal(loader);
}
}
