// Copyright (c) 2022  Binbin Zhang(binbzha@qq.com)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "api/wenet_api.h"

#include <memory>
#include <string>
#include <vector>

#include "decoder/asr_decoder.h"
#include "decoder/torch_asr_model.h"
#include "utils/json.h"
#include "utils/string.h"


class Recognizer {
 public:
  explicit Recognizer(const std::string& model_dir) {
    // FeaturePipeline init
    feature_config_ = std::make_shared<wenet::FeaturePipelineConfig>(80, 16000);
    feature_pipeline_ =
        std::make_shared<wenet::FeaturePipeline>(*feature_config_);
    // Resource init
    resource_ = std::make_shared<wenet::DecodeResource>();
    wenet::TorchAsrModel::InitEngineThreads();

    auto model = std::make_shared<wenet::TorchAsrModel>();
    model->Read(wenet::JoinPath(model_dir, "final.zip"));
    resource_->model = model;
    auto symbol_table = std::shared_ptr<fst::SymbolTable>(
        fst::SymbolTable::ReadText(wenet::JoinPath(model_dir, "words.txt")));
    resource_->symbol_table = symbol_table;
    resource_->unit_table = symbol_table;
    // Context config init
    context_config_ = std::make_shared<wenet::ContextConfig>();
    decode_options_ = std::make_shared<wenet::DecodeOptions>();
  }

  void Reset() {
    feature_pipeline_->Reset();
    decoder_->Reset();
    result_.clear();
  }

  void Decode(const char* data, int len, int last) {
    using wenet::DecodeState;
    // Init decoder when it is called first time
    if (decoder_ == nullptr) {
      // Optional init context graph
      if (context_.size() > 0) {
        context_config_->context_score = context_score_;
        auto context_graph =
            std::make_shared<wenet::ContextGraph>(*context_config_);
        context_graph->BuildContextGraph(context_, resource_->symbol_table);
        resource_->context_graph = context_graph;
      }
      decoder_ = std::make_shared<wenet::AsrDecoder>(feature_pipeline_,
          resource_, *decode_options_);
    }
    // Convert to 16 bits PCM data to float
    CHECK_EQ(len % 2, 0);
    std::vector<float> wav(len / 2, 0);
    for (int i = 0; i < wav.size(); i++) {
      wav[i] = *reinterpret_cast<const int16_t*>(data + i * 2);
    }
    feature_pipeline_->AcceptWaveform(wav);
    if (last > 0) {
      feature_pipeline_->set_input_finished();
    }

    while (true) {
      // TODO(Binbin Zhang): Process streaming call
      DecodeState state = decoder_->Decode(false);
      if (state == DecodeState::kWaitFeats) {
         break;
      } else if (state == DecodeState::kEndFeats) {
        UpdateResult(true);
        decoder_->Rescoring();
        UpdateResult(true);
        break;
      } else {
        // kEndBatch or kEndpoint(ignore it now)
        UpdateResult(false);
      }
    }
  }

  void UpdateResult(bool final_result) {
    json::JSON obj;
    obj["type"] = final_result ? "final_result" : "partial_result";
    int nbest = final_result ? nbest_ : 1;
    obj["nbest"] = json::Array();
    for (int i = 0; i < nbest && i < decoder_->result().size(); i++) {
      json::JSON one;
      one["sentence"] = decoder_->result()[i].sentence;
      if (final_result && enable_timestamp_) {
        one["word_pieces"] = json::Array();
        for (const auto& word_piece : decoder_->result()[i].word_pieces) {
          json::JSON piece;
          piece["word"] = word_piece.word;
          piece["start"] = word_piece.start;
          piece["end"] = word_piece.end;
          one["word_pieces"].append(piece);
        }
      }
      one["sentence"] = decoder_->result()[i].sentence;
      obj["nbest"].append(one);
    }
    result_ = obj.dump();
  }

  const char* GetResult() {
    return result_.c_str();
  }

  void set_nbest(int n) { nbest_ = n; }
  void set_enable_timestamp(bool flag) { enable_timestamp_ = flag; }
  void AddContext(const char* word) {
    context_.push_back(word);
  }
  void set_context_score(float score) { context_score_ = score; }

 private:
  // NOTE(Binbin Zhang): All use shared_ptr for clone in the future
  std::shared_ptr<wenet::FeaturePipelineConfig> feature_config_ = nullptr;
  std::shared_ptr<wenet::FeaturePipeline> feature_pipeline_ = nullptr;
  std::shared_ptr<wenet::DecodeResource> resource_ = nullptr;
  std::shared_ptr<wenet::DecodeOptions> decode_options_ = nullptr;
  std::shared_ptr<wenet::AsrDecoder> decoder_ = nullptr;
  std::shared_ptr<wenet::ContextConfig> context_config_ = nullptr;

  int nbest_ = 1;
  std::string result_;
  bool enable_timestamp_ = false;
  std::vector<std::string> context_;
  float context_score_;
};


void* wenet_init(const char* model_dir) {
  Recognizer* decoder = new Recognizer(model_dir);
  return reinterpret_cast<void*>(decoder);
}


void wenet_free(void* decoder) {
  delete reinterpret_cast<Recognizer *>(decoder);
}


void wenet_reset(void* decoder) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  recognizer->Reset();
}


void wenet_decode(void* decoder,
                  const char* data,
                  int len,
                  int last) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  recognizer->Decode(data, len, last);
}


const char* wenet_get_result(void* decoder) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  return recognizer->GetResult();
}


void wenet_set_log_level(int level) {
  FLAGS_logtostderr = true;
  FLAGS_v = level;
}


void wenet_set_nbest(void *decoder, int n) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  recognizer->set_nbest(n);
}


void wenet_set_timestamp(void *decoder, int flag) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  bool enable = flag > 0 ? true : false;
  recognizer->set_enable_timestamp(enable);
}


void wenet_add_context(void* decoder, const char* word) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  recognizer->AddContext(word);
}


void wenet_set_context_score(void *decoder, float score) {
  Recognizer *recognizer = reinterpret_cast<Recognizer *>(decoder);
  recognizer->set_context_score(score);
}
