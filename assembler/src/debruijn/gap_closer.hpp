//***************************************************************************
//* Copyright (c) 2011-2013 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * gap_closer.hpp
 *
 *  Created on: Oct 3, 2011
 *
 */

#ifndef GAP_CLOSER_HPP_
#define GAP_CLOSER_HPP_

#include <set>
#include <stack>
#include <type_traits>
#include <unordered_map>

#include "omni/omni_tools.hpp"
#include "standard.hpp"
#include "omni_labelers.hpp"
#include "io/easy_reader.hpp"
#include "dataset_readers.hpp"

#include "de/paired_info.hpp"

namespace debruijn_graph {

template<typename EdgeId>
Path<EdgeId> ConvertToPath(MappingPath<EdgeId> mp) {
  return mp.simple_path();
}
template<typename EdgeId>
Path<EdgeId> ConvertToPath(Path<EdgeId> mp) {
  return mp;
}

template<class Graph, class SequenceMapper>
class GapCloserPairedIndexFiller {
private:
  typedef typename Graph::EdgeId EdgeId;
  const Graph &graph_;
  const SequenceMapper& mapper_;

  map<EdgeId, pair<EdgeId, int> > OutTipMap;
  map<EdgeId, pair<EdgeId, int> > InTipMap;
  set<int> InTipsIds;
  set<int> OutTipsIds;

  size_t CorrectLength(Path<EdgeId> path, size_t idx) {
    size_t answer = graph_.length(path[idx]);
    if (idx == 0)
      answer -= path.start_pos();
    if (idx == path.size() - 1)
      answer -= graph_.length(path[idx]) - path.end_pos();
    return answer;
  }

  template<typename PairedRead>
  void ProcessPairedRead(omnigraph::PairedInfoIndexT<Graph> &paired_index,
      PairedRead& p_r) {
    Sequence read1 = p_r.first().sequence();
    Sequence read2 = p_r.second().sequence();

    Path<EdgeId> path1 = ConvertToPath(mapper_.MapSequence(read1));
    Path<EdgeId> path2 = ConvertToPath(mapper_.MapSequence(read2));
    for (size_t i = 0; i < path1.size(); ++i) {
      auto OutTipIter = OutTipMap.find(path1[i]);
      if (OutTipIter != OutTipMap.end()) {
        for (size_t j = 0; j < path2.size(); ++j) {
          auto InTipIter = InTipMap.find(path2[j]);
          if (InTipIter != InTipMap.end())
            paired_index.AddPairInfo(OutTipIter->second.first, InTipIter->second.first, 1000000., 1., 0.);
        }
      }
    }
  }

  void PrepareShiftMaps() {
    stack<pair<EdgeId, int>> edge_stack;
    for (auto iterator = graph_.ConstEdgeBegin(); !iterator.IsEnd();) {
      EdgeId edge = *iterator;
      if (graph_.IncomingEdgeCount(graph_.EdgeStart(edge)) == 0) {
        InTipMap.insert(make_pair(edge, make_pair(edge, 0)));
        edge_stack.push(make_pair(edge, 0));
        while (edge_stack.size() > 0) {
          pair<EdgeId, int> checking_pair = edge_stack.top();
          edge_stack.pop();
          if (graph_.IncomingEdgeCount(graph_.EdgeEnd(checking_pair.first)) == 1) {
            VertexId v = graph_.EdgeEnd(checking_pair.first);
            if (graph_.OutgoingEdgeCount(v)) {
              for (auto I = graph_.out_begin(v), E = graph_.out_end(v); I != E; ++I) {
                EdgeId Cur_edge = *I;
                InTipMap.insert(
                    make_pair(Cur_edge,
                        make_pair(edge,
                            graph_.length(checking_pair.first) + checking_pair.second)));
                edge_stack.push(
                    make_pair(Cur_edge,
                            graph_.length(checking_pair.first) + checking_pair.second));

              }
            }
          }
        }
      }

      if (graph_.OutgoingEdgeCount(graph_.EdgeEnd(edge)) == 0) {
        OutTipMap.insert(make_pair(edge, make_pair(edge, 0)));
        edge_stack.push(make_pair(edge, 0));
        while (edge_stack.size() > 0) {
          pair<EdgeId, int> checking_pair = edge_stack.top();
          edge_stack.pop();
          if (graph_.OutgoingEdgeCount(graph_.EdgeStart(checking_pair.first)) == 1) {
            if (graph_.IncomingEdgeCount(graph_.EdgeStart(checking_pair.first))) {
              vector<EdgeId> vec = graph_.IncomingEdges(
                  graph_.EdgeStart(checking_pair.first));
              for (size_t i = 0; i < vec.size(); i++) {
                EdgeId Cur_edge = vec[i];
                OutTipMap.insert(
                    make_pair(
                      Cur_edge,
                      make_pair(edge,
                        graph_.length(Cur_edge) + checking_pair.second)));
                edge_stack.push(
                    make_pair(Cur_edge,
                      graph_.length(Cur_edge) + checking_pair.second));
              }
            }
          }

        }
      }
      ++iterator;
    }
  }

  template<class PairedStream>
  void FillUsualIndex(omnigraph::PairedInfoIndexT<Graph> &paired_index, PairedStream& stream) {
    INFO("Processing paired reads (takes a while)");

    stream.reset();
    size_t n = 0;
    while (!stream.eof()) {
      typename PairedStream::read_type p_r;
      stream >> p_r;
      ProcessPairedRead(paired_index, p_r);
      VERBOSE_POWER(++n, " paired reads processed");
    }
  }

  template<class Streams>
  void FillParallelIndex(omnigraph::PairedInfoIndexT<Graph> &paired_index, Streams& streams) {
    INFO("Processing paired reads (takes a while)");

    size_t nthreads = streams.size();
    vector<omnigraph::PairedInfoIndexT<Graph>*> buffer_pi(nthreads);
    buffer_pi[0] = &paired_index;

    for (size_t i = 1; i < nthreads; ++i) {
      buffer_pi[i] = new omnigraph::PairedInfoIndexT<Graph>(graph_);
    }

    size_t counter = 0;
    #pragma omp parallel num_threads(nthreads)
    {
      #pragma omp for reduction(+ : counter)
      for (size_t i = 0; i < nthreads; ++i) {
        typedef typename Streams::ReaderType PairedStream;
        typename PairedStream::read_type r;
        PairedStream& stream = streams[i];
        stream.reset();

        while (!stream.eof()) {
          stream >> r;
          ++counter;
          ProcessPairedRead(*buffer_pi[i], r);
        }
      }
    }
    INFO("Used " << counter << " paired reads");

    INFO("Merging paired indices");
    for (size_t i = 1; i < nthreads; ++i) {
      buffer_pi[0]->AddAll(*(buffer_pi[i]));
      delete buffer_pi[i];
    }
  }

public:

  GapCloserPairedIndexFiller(const Graph &graph, const SequenceMapper& mapper) :
      graph_(graph), mapper_(mapper) {

  }

  /**
   * Method reads paired data from stream, maps it to genome and stores it in this PairInfoIndex.
   */
  template<class Streams>
  void FillIndex(omnigraph::PairedInfoIndexT<Graph> &paired_index, Streams& streams) {
    INFO("Preparing shift maps");
    PrepareShiftMaps();

    if (streams.size() == 1) {
      FillUsualIndex(paired_index, streams.back());
    } else {
      FillParallelIndex(paired_index, streams);
    }
  }

};

template<class Mapper>
bool CheckNoKmerClash(const Sequence& s, const Mapper& mapper) {
  vector<EdgeId> path = mapper.MapSequence(s).simple_path().sequence();
  return path.empty();
}

template<class Graph, class SequenceMapper>
class GapCloser {
 public:
  typedef function<bool (const Sequence&)> SequenceCheckF;
 private:
  typedef typename Graph::EdgeId EdgeId;
  typedef typename Graph::VertexId VertexId;
  typedef set<Point> Histogram;

  Graph& g_;
  int k_;
  PairedInfoIndexT<Graph>& tips_paired_idx_;
  const size_t min_intersection_;
  const size_t hamming_dist_bound_;
  const int init_gap_val_;
  const double weight_threshold_;
  SequenceCheckF sequence_check_f_;

  bool WeightCondition(const Histogram& infos) const {
    for (auto it = infos.begin(); it != infos.end(); ++it) {
//      VERIFY(math::eq(it->d, 100.));
      if (math::ge(it->weight, weight_threshold_))
        return true;
    }
    return false;
  }

  vector<size_t> DiffPos(const Sequence& s1, const Sequence& s2) const {
    VERIFY(s1.size() == s2.size());
    vector < size_t > answer;
    for (size_t i = 0; i < s1.size(); ++i)
      if (s1[i] != s2[i])
        answer.push_back(i);
    return answer;
  }

  size_t HammingDistance(const Sequence& s1, const Sequence& s2) const {
    VERIFY(s1.size() == s2.size());
    size_t dist = 0;
    for (size_t i = 0; i < s1.size(); ++i)
      if (s1[i] != s2[i])
        dist++;
    return dist;
  }

//  size_t HammingDistance(const Sequence& s1, const Sequence& s2) const {
//    return DiffPos(s1, s2).size();
//  }

  vector<size_t> PosThatCanCorrect(size_t overlap_length/*in nucls*/,
      const vector<size_t>& mismatch_pos, size_t edge_length/*in nucls*/,
      bool left_edge) const {
    TRACE("Try correct left edge " << left_edge);
    TRACE("Overlap length " << overlap_length);
    TRACE("Edge length " << edge_length);
    TRACE("Mismatches " << mismatch_pos);

    vector < size_t > answer;
    for (size_t i = 0; i < mismatch_pos.size(); ++i) {
      size_t relative_mm_pos =
          left_edge ?
              mismatch_pos[i] :
              overlap_length - 1 - mismatch_pos[i];
      if (overlap_length - relative_mm_pos + g_.k() < edge_length)
        //can correct mismatch
        answer.push_back(mismatch_pos[i]);
    }
    TRACE("Can correct mismatches: " << answer);
    return answer;
  }

  //todo write easier
  bool CanCorrectLeft(EdgeId e, int overlap, const vector<size_t>& mismatch_pos) const {
    return PosThatCanCorrect(overlap, mismatch_pos,
        g_.length(e) + g_.k(), true).size() == mismatch_pos.size();
  }

  //todo write easier
  bool CanCorrectRight(EdgeId e, int overlap,
      const vector<size_t>& mismatch_pos) const {
    return PosThatCanCorrect(overlap, mismatch_pos,
        g_.length(e) + g_.k(), false).size() == mismatch_pos.size();
  }

  bool MatchesEnd(const Sequence& long_seq, const Sequence& short_seq, bool from_begin) const {
    return from_begin ? long_seq.Subseq(0, short_seq.size()) == short_seq
        : long_seq.Subseq(long_seq.size() - short_seq.size()) == short_seq;
  }

  bool CorrectLeft(EdgeId first, EdgeId second, int overlap, const vector<size_t>& diff_pos) {
    DEBUG("Can correct first with sequence from second.");
    Sequence new_sequence = g_.EdgeNucls(first).Subseq(g_.length(first) - overlap + diff_pos.front(), g_.length(first) + k_ - overlap)
        + g_.EdgeNucls(second).First(k_);
    DEBUG("Checking new k+1-mers.");
    if (sequence_check_f_(new_sequence)) {
      DEBUG("Check ok.");
      DEBUG("Splitting first edge.");
      pair<EdgeId, EdgeId> split_res = g_.SplitEdge(first, g_.length(first) - overlap + diff_pos.front());
      first = split_res.first;
      tips_paired_idx_.RemoveEdgeInfo(split_res.second);
//              g_.DeleteEdge(first);
      DEBUG("Adding new edge.");
      VERIFY(MatchesEnd(new_sequence, g_.VertexNucls(g_.EdgeEnd(first)), true));
      VERIFY(MatchesEnd(new_sequence, g_.VertexNucls(g_.EdgeStart(second)), false));
      g_.AddEdge(g_.EdgeEnd(first), g_.EdgeStart(second),
          new_sequence);
      return true;
    } else {
      DEBUG("Check fail.");
      DEBUG("Filled k-mer already present in graph");
      return false;
    }
    return false;
  }

  bool CorrectRight(EdgeId first, EdgeId second, int overlap, const vector<size_t>& diff_pos) {
    DEBUG("Can correct second with sequence from first.");
    Sequence new_sequence = g_.EdgeNucls(first).Last(k_) + g_.EdgeNucls(second).Subseq(overlap, diff_pos.back() + 1 + k_);
    DEBUG("Checking new k+1-mers.");
    if (sequence_check_f_(new_sequence)) {
      DEBUG("Check ok.");
      DEBUG("Splitting second edge.");
      pair<EdgeId, EdgeId> split_res = g_.SplitEdge(second, diff_pos.back() + 1);
      second = split_res.second;
      tips_paired_idx_.RemoveEdgeInfo(split_res.first);
//              g_.DeleteEdge(second);
      DEBUG("Adding new edge.");
      VERIFY(MatchesEnd(new_sequence, g_.VertexNucls(g_.EdgeEnd(first)), true));
      VERIFY(MatchesEnd(new_sequence, g_.VertexNucls(g_.EdgeStart(second)), false));

      g_.AddEdge(g_.EdgeEnd(first), g_.EdgeStart(second),
          new_sequence);
      return true;
    } else {
      DEBUG("Check fail.");
      DEBUG("Filled k-mer already present in graph");
      return false;
    }
    return false;
  }

  bool HandlePositiveHammingDistanceCase(EdgeId first, EdgeId second, int overlap) {
    DEBUG("Match was imperfect. Trying to correct one of the tips");
    vector<size_t> diff_pos = DiffPos(g_.EdgeNucls(first).Last(overlap),
        g_.EdgeNucls(second).First(overlap));
    if (CanCorrectLeft(first, overlap, diff_pos)) {
      return CorrectLeft(first, second, overlap, diff_pos);
    } else if (CanCorrectRight(second, overlap, diff_pos)) {
      return CorrectRight(first, second, overlap, diff_pos);
    } else {
      DEBUG("Can't correct tips due to the graph structure");
      return false;
    }
  }

  bool HandleSimpleCase(EdgeId first, EdgeId second, int overlap) {
    DEBUG("Match was perfect. No correction needed");
    //strange info guard
    VERIFY(overlap <= k_);
    if (overlap == k_) {
      DEBUG("Tried to close zero gap");
      return false;
    }
    //old code
    Sequence edge_sequence = g_.EdgeNucls(first).Last(k_)
        + g_.EdgeNucls(second).Subseq(overlap, k_);
    if (sequence_check_f_(edge_sequence)) {
      DEBUG(
        "Gap filled: Gap size = " << k_ - overlap << "  Result seq "
            << edge_sequence.str());
      g_.AddEdge(g_.EdgeEnd(first), g_.EdgeStart(second), edge_sequence);
      return true;
    } else {
      DEBUG("Filled k-mer already present in graph");
      return false;
    }
  }

  bool ProcessPair(EdgeId first, EdgeId second) {
    TRACE("Processing edges " << g_.str(first) << " and " << g_.str(second));
    TRACE("first " << g_.EdgeNucls(first) << " second " << g_.EdgeNucls(second));

    if (first == g_.conjugate(second)) {
        INFO("Trying to join conjugate edges " << g_.int_id(first));
        return false;
    }
    //may be negative!
    int gap = max(init_gap_val_,
        -1 * (int)(min(g_.length(first), g_.length(second)) - 1));

    Sequence seq1 = g_.EdgeNucls(first);
    Sequence seq2 = g_.EdgeNucls(second);
    TRACE("Checking possible gaps from " << gap << " to " << k_ - min_intersection_);
    for (; gap <= k_ - (int)min_intersection_; ++gap) {
      int overlap = k_ - gap;
      size_t hamming_distance = HammingDistance(g_.EdgeNucls(first).Last(overlap)
          , g_.EdgeNucls(second).First(overlap));
      if (hamming_distance <= hamming_dist_bound_) {
        DEBUG("For edges " << g_.str(first) << " and " << g_.str(second)
            << ". For gap value " << gap << " (overlap " << overlap << "bp) hamming distance was " << hamming_distance);
//        DEBUG("Sequences of distance " << tip_distance << " :"
//                << seq1.Subseq(seq1.size() - k).str() << "  "
//                << seq2.Subseq(0, k).str());

        if (hamming_distance > 0) {
          return HandlePositiveHammingDistanceCase(first, second, overlap);
        } else {
          return HandleSimpleCase(first, second, overlap);
        }
      }
    }
    return false;
  }

public:
  void CloseShortGaps() {
    INFO("Closing short gaps");
    int gaps_filled = 0;
    int gaps_checked = 0;
    for (auto it = g_.SmartEdgeBegin(); !it.IsEnd(); ++it) {
      EdgeId first_edge = *it;
      const InnerMap<Graph>& inner_map = tips_paired_idx_.GetEdgeInfo(*it, 0);
      for (auto inner_it = inner_map.Begin(); inner_it != inner_map.End(); ++inner_it) {
        EdgeId second_edge = (*inner_it).first;
        const Point& point = (*inner_it).second;
          if (first_edge != second_edge && math::ge(point.weight, weight_threshold_)) {
            if (!g_.IsDeadEnd(g_.EdgeEnd(first_edge)) || !g_.IsDeadStart(g_.EdgeStart(second_edge))) {
              // WARN("Topologically wrong tips");
            continue;
          }
            ++gaps_checked;
            if (ProcessPair(first_edge, second_edge)) {
              ++gaps_filled;
            break;
          }
        }
      }
    }

    INFO("Closing short gaps complete: filled " << gaps_filled
            << " gaps after checking " << gaps_checked
            << " candidates");
    omnigraph::Compressor<Graph> compressor(g_);
    compressor.CompressAllVertices();
  }

  GapCloser(Graph& g, PairedInfoIndexT<Graph>& tips_paired_idx,
            size_t min_intersection, double weight_threshold,
            const SequenceCheckF& sequence_check_f,
            size_t hamming_dist_bound = 0 /*min_intersection_ / 5*/)
          : g_(g),
            k_((int) g_.k()),
            tips_paired_idx_(tips_paired_idx),
            min_intersection_(min_intersection),
            hamming_dist_bound_(hamming_dist_bound),
            init_gap_val_(-10),
            weight_threshold_(weight_threshold),
            sequence_check_f_(sequence_check_f) {
    VERIFY(min_intersection_ < g_.k());
    DEBUG("weight_threshold=" << weight_threshold_);
    DEBUG("min_intersect=" << min_intersection_);
    DEBUG("paired_index size=" << tips_paired_idx_.size());
  }

private:
  DECL_LOGGER("GapCloser");
};

template<class Streams>
void CloseGaps(conj_graph_pack& gp, Streams& streams) {
  typedef NewExtendedSequenceMapper<Graph, Index> Mapper;
  auto mapper = MapperInstance(gp);
  GapCloserPairedIndexFiller<Graph, Mapper> gcpif(
      gp.g, *mapper);
  PairedIndexT tips_paired_idx(gp.g);
//  if (fileExists("tip_info.prd")) {
//    ConjugateDataScanner<Graph> scanner(gp.g, gp.int_ids);
//    scanner.loadPaired("tip_info", tips_paired_idx);
//  } else {
  gcpif.FillIndex(tips_paired_idx, streams);
//    ConjugateDataPrinter<Graph> printer(gp.g, gp.int_ids);
//    printer.savePaired("tip_info", tips_paired_idx);
//  }
  GapCloser<Graph, Mapper> gap_closer(gp.g, tips_paired_idx,
                                      cfg::get().gc.minimal_intersection, cfg::get().gc.weight_threshold,
                                      boost::bind(&CheckNoKmerClash<Mapper>, _1, boost::ref(*mapper)));
  gap_closer.CloseShortGaps();
}

void CloseGaps(conj_graph_pack& gp) {
  INFO("SUBSTAGE == Closing gaps");

  //TODO: discuss what to use for gap closing
  size_t lib_index = 0;
  for (size_t i = 0; i < cfg::get().ds.reads.lib_count(); ++i) {
     if (cfg::get().ds.reads[i].type() == io::LibraryType::PairedEnd) {
         lib_index = i;
         break;
     }
  }
  if (cfg::get().use_multithreading) {
    auto streams = paired_binary_readers(cfg::get().ds.reads[lib_index], true, 0);

    CloseGaps(gp, *streams);

  } else {
    auto_ptr<PairedReadStream> stream = paired_easy_reader(cfg::get().ds.reads[lib_index], true, 0);
    io::ReadStreamVector <PairedReadStream> streams(stream.get());
    //todo WTF what does it mean?
    streams.release();
    CloseGaps(gp, streams);
  }

}

}

#endif /* GAP_CLOSER_HPP_ */
