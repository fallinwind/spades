//***************************************************************************
//* Copyright (c) 2011-2012 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * main.cpp
 *
 *  Created on: 08.07.2011
 *      Author: snikolenko
 */
 
#include "standard.hpp"
#include "logger/logger.hpp"

#include <cmath>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <map>
#include <list>
#include <queue>
#include <cstdarg>
#include <algorithm>
#include <cassert>
#include <unordered_set>
#include <boost/filesystem.hpp>


#include "segfault_handler.hpp"
#include "config_struct_hammer.hpp"
#include "read/ireadstream.hpp"
#include "hammer_tools.hpp"
#include "kmer_cluster.hpp"
#include "position_kmer.hpp"
#include "globals.hpp"

#include "memory_limit.hpp"

// forking
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
// file size
#include <sys/stat.h>


using std::string;
using std::vector;
using std::map;

std::vector<std::string> Globals::input_filenames = std::vector<std::string>();
std::vector<std::string> Globals::input_filename_bases = std::vector<std::string>();
std::vector<hint_t> Globals::input_file_blob_positions = std::vector<hint_t>();
std::vector<size_t> Globals::input_file_sizes = std::vector<size_t>();
std::vector<uint32_t> * Globals::subKMerPositions = NULL;
std::vector<hint_t> * Globals::kmernos = NULL;
std::vector<KMerCount> * Globals::kmers = NULL;
int Globals::iteration_no = 0;
hint_t Globals::revNo = 0;

hint_t Globals::blob_size = 0;
hint_t Globals::blob_max_size = 0;
hint_t Globals::number_of_kmers = 0;
char * Globals::blob = NULL;
char * Globals::blobquality = NULL;
char Globals::char_offset = 0;

bool Globals::use_common_quality = false;
char Globals::common_quality = 0;
double Globals::common_kmer_errprob = 0;
double Globals::quality_probs[256] = { 0 };
double Globals::quality_lprobs[256] = { 0 };

std::vector<PositionRead> * Globals::pr = NULL;

int main(int argc, char * argv[]) {

	segfault_handler sh;

	try
	{

	TIMEDLN("Hey there");
	string config_file = CONFIG_FILENAME;
	if (argc > 1) config_file = argv[1];
	TIMEDLN("Loading config from " << config_file.c_str());
	cfg::create_instance(config_file);


	// general config parameters
	Globals::use_common_quality = cfg::get().common_quality > 0;
	Globals::common_quality = (char)cfg::get().common_quality;
	Globals::common_kmer_errprob = 1.0;
	for (size_t i=0; i<K; ++i)
		Globals::common_kmer_errprob *= 1 - pow(10.0, - Globals::common_quality / 10.0);
	Globals::common_kmer_errprob = 1 - Globals::common_kmer_errprob;

	// hard memory limit
    const size_t GB = 1 << 30;
    limit_memory(cfg::get().general_hard_memory_limit * GB);

    // input files with reads
    if (cfg::get().input_paired_1 != "" && cfg::get().input_paired_2 != "") {
    	Globals::input_filenames.push_back(cfg::get().input_paired_1);
    	Globals::input_filenames.push_back(cfg::get().input_paired_2);
    }
    if (cfg::get().input_single != "") Globals::input_filenames.push_back(cfg::get().input_single);

    VERIFY(Globals::input_filenames.size() > 0);

    for (size_t iFile=0; iFile < Globals::input_filenames.size(); ++iFile) {
    	Globals::input_filename_bases.push_back(
    				boost::filesystem::basename(boost::filesystem::path(Globals::input_filenames[iFile])) +
    				boost::filesystem::extension(boost::filesystem::path(Globals::input_filenames[iFile]))
    	);
    	cout << Globals::input_filename_bases[iFile] << endl;
    }

    // decompress input reads if they are gzipped
	HammerTools::DecompressIfNeeded();

	// determine quality offset if not specified
    if (!cfg::get().input_qvoffset_opt) {
    	cout << "Trying to determine PHRED offset" << endl;
    	int determined_offset = determine_offset(Globals::input_filenames.front());
    	if (determined_offset < 0) {
    		cout << "Failed to determine offset! Specify it manually and restart, please!" << endl;
    		return 0;
    	} else {
    		cout << "Determined value is " << determined_offset << endl;
    		cfg::get_writable().input_qvoffset = determined_offset;
    	}
    } else {
    	cfg::get_writable().input_qvoffset = *cfg::get().input_qvoffset_opt;
    }
    Globals::char_offset = (char)cfg::get().input_qvoffset;

    // Pre-cache quality probabilities
    for (unsigned qual = 0; qual < sizeof(Globals::quality_probs) / sizeof(Globals::quality_probs[0]); ++qual) {
      Globals::quality_probs[qual] = (qual < 3 ? 0.25 : 1 - pow(10.0, -(int)qual / 10.0));
      Globals::quality_lprobs[qual] = log(Globals::quality_probs[qual]);
    }

    // if we need to change single Ns to As, this is the time
    if (cfg::get().general_change_n_to_a && cfg::get().count_do) {
    	TIMEDLN("Changing single Ns to As in input read files.");
    	HammerTools::ChangeNtoAinReadFiles();
    	TIMEDLN("Single Ns changed, " << Globals::input_filenames.size() << " read files written.");
    }

    // estimate total read size
    hint_t totalReadSize = HammerTools::EstimateTotalReadSize();
	TIMEDLN("Estimated total size of all reads is " << totalReadSize);

	// allocate the blob
	Globals::blob_size = totalReadSize + 1;
	Globals::blob_max_size = (hint_t)(Globals::blob_size * ( 2 + cfg::get().general_blob_margin));
	Globals::blob = new char[ Globals::blob_max_size ];
	if (!Globals::use_common_quality) Globals::blobquality = new char[ Globals::blob_max_size ];
	TIMEDLN("Max blob size as allocated is " << Globals::blob_max_size);

	// initialize subkmer positions
	HammerTools::InitializeSubKMerPositions();

	int max_iterations = cfg::get().general_max_iterations;
	if (HammerTools::doingMinimizers()) max_iterations = max_iterations+7;

	// now we can begin the iterations
	for (Globals::iteration_no = 0; Globals::iteration_no < max_iterations; ++Globals::iteration_no) {
		cout << "\n     === ITERATION " << Globals::iteration_no << " begins ===" << endl;
		bool do_everything = cfg::get().general_do_everything_after_first_iteration && (Globals::iteration_no > 0);

		// initialize k-mer structures
		Globals::kmernos = new std::vector<hint_t>;
		Globals::kmers = new std::vector<KMerCount>;

		// read input reads into blob
		Globals::pr = new vector<PositionRead>();
		HammerTools::ReadAllFilesIntoBlob();

    // count k-mers
    if (cfg::get().count_do || cfg::get().sort_do || do_everything ) {
      HammerTools::CountKMersBySplitAndMerge();
    } else {
      {
        ifstream is(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.numbers.ser"), ios::binary);
        Globals::kmernos->clear();
        TIMEDLN("Reading serialized kmernos.");
        size_t sz;
        is.read((char*)&sz, sizeof(sz));
        Globals::kmernos->resize(sz);
        is.read((char*)&(*Globals::kmernos)[0], sz*sizeof((*Globals::kmernos)[0]));
        TIMEDLN("Read serialized kmernos.");
      }
      HammerTools::RemoveFile(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.numbers.ser"));
    }
  
    // fill in already prepared k-mers
    if (!do_everything && cfg::get().input_read_solid_kmers) {
      TIMEDLN("Loading k-mers from " << cfg::get().input_solid_kmers );
      HammerTools::ReadKmersWithChangeToFromFile(cfg::get().input_solid_kmers, Globals::kmers, Globals::kmernos );
      TIMEDLN("K-mers loaded.");
    }
  
    // cluster and subcluster the Hamming graph
    if (cfg::get().hamming_do || do_everything) {
      std::vector<std::vector<int> > classes;

      unionFindClass uf(Globals::kmernos->size() + 1);
      KMerHamClusterer clusterer(cfg::get().general_tau);
      TIMEDLN("Clustering Hamming graph.");
      clusterer.cluster(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamcls"),
                        *Globals::kmernos, uf);
      uf.get_classes(classes);
      size_t num_classes = classes.size();

#if 0
      struct UfCmp {
        bool operator()(const std::vector<int> &lhs, const std::vector<int> &rhs) {
          return lhs[0] < rhs[0];
        }
      };

      std::sort(classes.begin(), classes.end(),  UfCmp());
      for (size_t i = 0; i < classes.size(); ++i) {
        std::cerr << i << ": { ";
        for (size_t j = 0; j < classes[i].size(); ++j)
          std::cerr << classes[i][j] << ", ";
        std::cerr << "}" << std::endl;
      }
#endif
      TIMEDLN("Clustering done. Total clusters: " << num_classes);

      TIMEDLN("Writing down clusters.");
      std::string fname = HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.hamming");
      std::ofstream ofs(fname, std::ios::binary | std::ios::out);

      for (size_t i=0; i < classes.size(); ++i ) {
        size_t sz = classes[i].size();
        ofs.write((char*)&i, sizeof(i));
        ofs.write((char*)&sz, sizeof(sz));

        for (size_t j=0; j < classes[i].size(); ++j) {
          int cls = classes[i][j];
          ofs.write((char*)&cls, sizeof(cls));
        }
        classes[i].clear();
      }
      classes.clear();
      ofs.close();
      TIMEDLN("Clusters written.");
    }

    if (cfg::get().bayes_do || do_everything) {
      TIMEDLN("Subclustering Hamming graph");
      int clustering_nthreads = min(cfg::get().general_max_nthreads, cfg::get().bayes_nthreads);
      KMerClustering kmc(Globals::kmers, Globals::kmernos, clustering_nthreads, cfg::get().general_tau);
      boost::shared_ptr<FOStream> ofkmers = cfg::get().hamming_write_solid_kmers ?
                                            FOStream::init(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.solid").c_str()) : boost::shared_ptr<FOStream>();
      boost::shared_ptr<FOStream> ofkmers_bad = cfg::get().hamming_write_bad_kmers ?
                                                FOStream::init(HammerTools::getFilename(cfg::get().input_working_dir, Globals::iteration_no, "kmers.bad").c_str()) : boost::shared_ptr<FOStream>();
      kmc.process(ofkmers, ofkmers_bad);
      TIMEDLN("Finished clustering.");
    }

		// expand the set of solid k-mers (with minimizer iterations, we don't need it)
		if ((cfg::get().expand_do || do_everything) && !HammerTools::doingMinimizers() ) {
			int expand_nthreads = min( cfg::get().general_max_nthreads, cfg::get().expand_nthreads);
			TIMEDLN("Starting solid k-mers expansion in " << expand_nthreads << " threads.");
			for ( int expand_iter_no = 0; expand_iter_no < cfg::get().expand_max_iterations; ++expand_iter_no ) {
				size_t res = HammerTools::IterativeExpansionStep(expand_iter_no, expand_nthreads, *Globals::kmers);
				TIMEDLN("Solid k-mers iteration " << expand_iter_no << " produced " << res << " new k-mers.");
				if ( res < 10 ) break;
			}
			TIMEDLN("Solid k-mers finalized.");
		}

		hint_t totalReads = 0;
		// reconstruct and output the reads
		if ( cfg::get().correct_do || do_everything ) {
			totalReads = HammerTools::CorrectAllReads();
		}

		// prepare the reads for next iteration
		// delete consensuses, clear kmer data, and restore correct revcomps
		delete Globals::kmernos;
		delete Globals::kmers;
		delete Globals::pr;

		if (totalReads < 1 && !HammerTools::doingMinimizers() ) {
			TIMEDLN("Too few reads have changed in this iteration. Exiting.");
			break;
		}
		// break;
	}

	// clean up
	Globals::subKMerPositions->clear();
	delete Globals::subKMerPositions;
	delete [] Globals::blob;
	delete [] Globals::blobquality;

	TIMEDLN("All done. Exiting.");
	}
	catch (std::bad_alloc const& e)
	{
		std::cerr << "Not enough memory to run BayesHammer. " << e.what() << std::endl;
	    return EINTR;
	}
	/*catch (std::exception const& e)
	{
	    std::cerr << "Exception caught " << e.what() << std::endl;
	    return EINTR;
	}
	catch (...)
	{
	    std::cerr << "Unknown exception caught " << std::endl;
	    return EINTR;
      }*/

	return 0;
}


