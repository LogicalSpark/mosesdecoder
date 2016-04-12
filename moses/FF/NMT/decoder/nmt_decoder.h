#pragma once

#include <string>
#include <memory>
#include <algorithm>
#include <functional>

#include <thrust/functional.h>
#include <thrust/device_vector.h>
#include <thrust/host_vector.h>
#include <thrust/device_ptr.h>
#include <thrust/extrema.h>
#include <thrust/sort.h>
#include <thrust/sequence.h>

#include "common/vocab.h"
#include "bahdanau/encoder.h"
#include "bahdanau/decoder.h"
#include "bahdanau/model.h"
#include "common/utils.h"
#include "mblas/matrix.h"
#include "decoder/hypothesis_manager.h"


using namespace thrust::placeholders;

class NMTDecoder {
  using Words = std::vector<size_t>;
  using Hypotheses = std::vector<Hypothesis>;
 public:
   NMTDecoder(
       std::shared_ptr<Weights> model,
       std::shared_ptr<Vocab> srcVocab,
       std::shared_ptr<Vocab> trgVocab,
       const size_t beamSize=50)
       : model_(model),
         srcVocab_(srcVocab),
         trgVocab_(trgVocab),
         encoder_(new Encoder(*model_)),
         decoder_(new Decoder(*model_)),
         beamSize_(beamSize),
         Costs_() {
   }

   Words translate(std::string& sentence) {
     size_t sourceSentenceLength = prepareSourceSentence(sentence);
     prepareDecoder();

     size_t batchSize = beamSize_;
     Costs_.Resize(1,1);
     HypothesisManager hypoManager(batchSize, (*trgVocab_)["</s>"]);

     for(size_t len = 0; len < 3 * sourceSentenceLength; ++len) {
       std::vector<size_t> bestWordIndices, bestWordHyps;
       decoder_->GetProbs(Probs_, AlignedSourceContext_,
                          PrevState_, PrevEmbedding_, SourceContext_);

       auto bestHypos = GetBestExtensions(batchSize);
       hypoManager.AddHypotheses(bestHypos);

       for (auto& best: bestHypos) {
         if (best.GetWord() != (*trgVocab_)["</s>"]) {
           bestWordIndices.push_back(best.GetWord());
           bestWordHyps.push_back(best.GetPrevStateIndex());
         } else {
           --batchSize;
         }
       }

       if (batchSize <= 0) break;
       Costs_.Resize(1, batchSize);

       decoder_->Lookup(Embedding_, bestWordIndices);
       Assemble(BestState_, PrevState_, bestWordHyps);
       decoder_->GetNextState(State_, Embedding_,
                              BestState_, AlignedSourceContext_);

       mblas::Swap(State_, PrevState_);
       mblas::Swap(Embedding_, PrevEmbedding_);
     }

     return hypoManager.GetBestTranslation();
   }
 private:
   size_t prepareSourceSentence(std::string& sentence) {
     Trim(sentence);
     std::vector<std::string> tokens;
     Split(sentence, tokens, " ");
     auto encoded_tokens = srcVocab_->Encode(tokens, true);
     encoder_->GetContext(encoded_tokens, SourceContext_);
     return encoded_tokens.size();
   }

   Hypotheses GetBestExtensions(size_t batchSize) {
     Hypotheses hypos;
     Element(Log(_1), Probs_);
     Broadcast(_1 + _2, Transpose(Probs_), Costs_);
     Transpose(Probs_);
     size_t kk = Probs_.Cols() * Probs_.Rows();
     thrust::device_vector<int> keys(kk);
     thrust::sequence(keys.begin(), keys.end());
     thrust::sort_by_key(Probs_.begin(), Probs_.end(), keys.begin());
     thrust::host_vector<int> bestKeys(keys.end() - batchSize, keys.end());
     std::vector<float> bestCosts(batchSize);

     Costs_.Resize(batchSize, 1 );
     HypothesisManager hypoManager(batchSize, (*trgVocab_)["</s>"]);
     for (size_t i = 0; i < bestKeys.size(); ++i) {
       Costs_.GetVec()[i] = Probs_.GetVec()[kk - batchSize + i];

       hypos.emplace_back(bestKeys[i] % Probs_.Cols(), bestKeys[i] / Probs_.Cols(), Probs_.GetVec()[kk - batchSize + i]);
     }
     return hypos;

   }

   void prepareDecoder() {
     decoder_->EmptyState(PrevState_, SourceContext_, 1);
     decoder_->EmptyEmbedding(PrevEmbedding_, 1);
   }

 protected:
    std::shared_ptr<Weights> model_;
    std::shared_ptr<Vocab> srcVocab_;
    std::shared_ptr<Vocab> trgVocab_;
    std::shared_ptr<Encoder> encoder_;
    std::shared_ptr<Decoder> decoder_;
    const size_t beamSize_;
    mblas::Matrix SourceContext_;
    mblas::Matrix PrevState_;
    mblas::Matrix PrevEmbedding_;
    mblas::Matrix BestState_;
    mblas::Matrix Costs_;

    mblas::Matrix AlignedSourceContext_;
    mblas::Matrix Probs_;

    mblas::Matrix State_;
    mblas::Matrix Embedding_;

};
