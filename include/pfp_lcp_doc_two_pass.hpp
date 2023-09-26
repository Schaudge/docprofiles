/*
 * File: pfp_lcp_doc_two_pass.hpp
 * Description: ....
 * Date: September 17th, 2022
 */

#ifndef _LCP_PFP_DOC_TWO_PASS_HH
#define _LCP_PFP_DOC_TWO_PASS_HH

#include <common.hpp>
#include <iostream>
#include <sdsl/rmq_support.hpp>
#include <sdsl/int_vector.hpp>
extern "C" {
#include <gsacak.h>
}
#include <pfp.hpp>
#include <ref_builder.hpp>
#include <deque>
#include <vector>
#include <bits/stdc++.h>
#include <omp.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <emmintrin.h>
#include <cstdio>

#define TEMPDATA_RECORD 6

// macros for reading from file
#define GET_IS_START(fd, num) (0x2 & fd[num * TEMPDATA_RECORD]) >> 1
#define GET_IS_END(fd, num) (0x1 & fd[num * TEMPDATA_RECORD])
#define GET_BWT_CH(fd, num) (fd[num * TEMPDATA_RECORD + 1])
#define GET_DOC_OF_LF(fd, num) ((0xFF & fd[num * TEMPDATA_RECORD + 2]) | ((0xFF & fd[num * TEMPDATA_RECORD + 3]) << 8))
#define GET_LCP(fd, num) ((0xFF & fd[num * TEMPDATA_RECORD + 4]) | ((0xFF & fd[num * TEMPDATA_RECORD + 5]) << 8))
#define GET_DAP(fd, num) ((0xFF & fd[num * DOCWIDTH]) | ((0xFF & fd[num * DOCWIDTH + 1]) << 8))

// struct for temp lcp queue data
typedef struct
{
    bool is_start = false;
    bool is_end = false;
    uint8_t bwt_ch = 0;
    size_t doc_num = 0;
    size_t lcp_i = 0;
} temp_data_entry_t;

class pfp_lcp_doc_two_pass {
    public:
        pf_parsing& pf;
        std::vector<size_t> min_s; // Value of the minimum lcp_T in each run of BWT_T
        std::vector<size_t> pos_s; // Position of the minimum lcp_T in each run of BWT_T

        uint8_t head; // character of current BWT run
        size_t length = 0; // length of the current BWT run

        bool rle; // run-length encode the BWT
        size_t total_num_runs = 0;
        size_t NUMCOLSFORTABLE = 0; 

        size_t tmp_file_size = 0;
        size_t max_dap_records = 0;
        size_t max_lcp_records = 0;

        pfp_lcp_doc_two_pass(pf_parsing &pfp_, std::string filename, RefBuilder* ref_build, 
                            std::string temp_prefix, size_t tmp_size, bool rle_ = true) : 
                pf(pfp_),
                min_s(1, pf.n),
                pos_s(1,0),
                head(0),
                num_docs(ref_build->num_docs),
                ch_doc_encountered(256, std::vector<bool>(ref_build->num_docs, false)),
                rle(rle_),
                tmp_file_size(tmp_size)
    {   
        /* construction algorithm for document array profiles */
        STATUS_LOG("build_main", "building bwt and doc profiles based on pfp (1st-pass)");
        auto start = std::chrono::system_clock::now();

        initialize_index_files(filename, temp_prefix);  
        initialize_tmp_size_variables();
        assert(pf.dict.d[pf.dict.saD[0]] == EndOfDict);

        // variables for bwt/lcp/sa construction
        phrase_suffix_t curr;
        phrase_suffix_t prev;

        // variables for doc profile construction 
        uint8_t prev_bwt_ch = 0;
        size_t curr_run_num = 0;
        size_t pos = 0;
        std::vector<size_t> curr_da_profile (ref_build->num_docs, 0);

        // create a predecessor max lcp table, and re-initialize with max_lcp
        num_blocks_of_32 = num_docs/32;
        num_blocks_of_32++;
        uint16_t max_lcp_init = 0;
        
        // create a separate block of memory for each character
        predecessor_max_lcp = new uint16_t*[256];
        for (size_t i = 0; i < 256; i++) {
            predecessor_max_lcp[i] =  new uint16_t[num_blocks_of_32 * 32];
        }
        for (size_t i = 0; i < 256; i++)
            for (size_t j = 0; j < num_blocks_of_32 * 32; j++)
                predecessor_max_lcp[i][j] = max_lcp_init;

        // create a struct to store data for temp file
        temp_data_entry_t curr_data_entry;

        // Start of the construction ...
        inc(curr);
        while (curr.i < pf.dict.saD.size())
        {
            // Make sure current suffix is a valid proper phrase suffix 
            // (at least w characters but not whole phrase)
            if(is_valid(curr))
            {
                // Compute the next character of the BWT of T
                std::vector<phrase_suffix_t> same_suffix(1, curr);
                phrase_suffix_t next = curr;

                // Go through suffix array of dictionary and store all phrase ids with same suffix
                while (inc(next) && (pf.dict.lcpD[next.i] >= curr.suffix_length))
                {
                    assert(next.suffix_length >= curr.suffix_length);
                    assert((pf.dict.b_d[next.sn] == 0 && next.suffix_length >= pf.w) || (next.suffix_length != curr.suffix_length));
                    if (next.suffix_length == curr.suffix_length)
                    {
                        same_suffix.push_back(next);
                    }
                }

                // Hard case: phrases with different BWT characters precediing them
                int_t lcp_suffix = compute_lcp_suffix(curr, prev);
                typedef std::pair<int_t *, std::pair<int_t *, uint8_t>> pq_t;

                // using lambda to compare elements.
                auto cmp = [](const pq_t &lhs, const pq_t &rhs) {
                    return *lhs.first > *rhs.first;
                };
                
                // Merge a list of occurrences of each phrase in the BWT of the parse
                std::priority_queue<pq_t, std::vector<pq_t>, decltype(cmp)> pq(cmp);
                for (auto s: same_suffix)
                {
                    size_t begin = pf.pars.select_ilist_s(s.phrase + 1);
                    size_t end = pf.pars.select_ilist_s(s.phrase + 2);
                    pq.push({&pf.pars.ilist[begin], {&pf.pars.ilist[end], s.bwt_char}});
                }

                size_t prev_occ;
                bool first = true;
                while (!pq.empty())
                {
                    auto curr_occ = pq.top();
                    pq.pop();

                    if (!first)
                    {
                        // Compute the minimum s_lcpP of the the current and previous occurrence of the phrase in BWT_P
                        lcp_suffix = curr.suffix_length + min_s_lcp_T(*curr_occ.first, prev_occ);
                    }
                    first = false;

                    // Update min_s
                    print_lcp(lcp_suffix, j);
                    update_ssa(curr, *curr_occ.first);
                    update_bwt(curr_occ.second.second, 1);
                    update_esa(curr, *curr_occ.first);

                    ssa = (pf.pos_T[*curr_occ.first] - curr.suffix_length) % (pf.n - pf.w + 1ULL);
                    esa = (pf.pos_T[*curr_occ.first] - curr.suffix_length) % (pf.n - pf.w + 1ULL);

                    /* Start of DA Code */
                    curr_bwt_ch = curr_occ.second.second;
                    size_t lcp_i = lcp_suffix;
                    size_t sa_i = ssa;
                    size_t doc_i = ref_build->doc_ends_rank(ssa);

                    // Determine whether current suffix is a run boundary   
                    bool is_start = (pos == 0 || curr_bwt_ch != prev_bwt_ch) ? 1 : 0;
                    bool is_end = (pos == ref_build->total_length-1); // only special case, common case is below

                    // Handle scenario where the previous suffix was a end of a run
                    if (pos > 0 && prev_bwt_ch != curr_bwt_ch)
                        curr_data_entry.is_end = true;
                    
                    // Push previous suffix data into temp data file
                    if (pos > 0) {
                        write_data_to_temp_file(curr_data_entry);
                        if (curr_data_entry.is_start || curr_data_entry.is_end)
                            write_profile_to_temp_file(curr_da_profile);
                    }
                    
                    if (is_start) {curr_run_num++;}
                    size_t pos_of_LF_i = (sa_i > 0) ? (sa_i - 1) : (ref_build->total_length-1);
                    size_t doc_of_LF_i = ref_build->doc_ends_rank(pos_of_LF_i);

                    // std::cout << "run_num = " << curr_run_num <<  ", ch = " << curr_bwt_ch << ", doc = " << doc_of_LF_i << ", lcp = " << lcp_i << ", suffix_length = " << (ref_build->total_length - pos_of_LF_i)  << std::endl;
                    // if (curr_run_num > 30)
                    //     std::exit(1);

                    // Gather current suffix data
                    curr_data_entry = {is_start, is_end, curr_bwt_ch, doc_of_LF_i, lcp_i};

                    // Update matrices based on current BWT character, and doc
                    ch_doc_encountered[curr_bwt_ch][doc_of_LF_i] = true;

                    // Re-initialize doc profiles and max lcp with current document (itself)
                    std::fill(curr_da_profile.begin(), curr_da_profile.end(), 0);
                    curr_da_profile[doc_of_LF_i] = ref_build->total_length - pos_of_LF_i;

                    // Update the predecessor table, and initialize current document array profile
                    update_predecessor_max_lcp_table(lcp_i, ref_build->total_length, pos_of_LF_i, doc_of_LF_i, curr_bwt_ch);
                    initialize_current_row_profile(doc_of_LF_i, curr_da_profile, curr_bwt_ch);

                    /* End of DA Code */

                    // Update prevs
                    prev_occ = *curr_occ.first;
                    prev_bwt_ch = curr_bwt_ch;

                    // Update pq
                    curr_occ.first++;
                    if (curr_occ.first != curr_occ.second.first)
                        pq.push(curr_occ);

                    j += 1;
                    pos += 1;
                }
                prev = same_suffix.back();
                curr = next;
            }
            else {
                inc(curr);
            }
        }
        DONE_LOG((std::chrono::system_clock::now() - start));

        // make sure to write the last suffix to temp data
        write_data_to_temp_file(curr_data_entry);
        if (curr_data_entry.is_start || curr_data_entry.is_end)
            write_profile_to_temp_file(curr_da_profile);

        // Re-initialize the predecessor table, and other structures
        for (size_t i = 0; i < 256; i++) {
            for (size_t j = 0; j < num_blocks_of_32 * 32; j++)
                predecessor_max_lcp[i][j] = max_lcp_init;
            for (size_t j = 0; j < num_docs; j++)
                ch_doc_encountered[i][j] = false;
        }

        // Perform 2nd pass ...
        STATUS_LOG("build_main", "performing 2nd pass to update profiles");
        start = std::chrono::system_clock::now();

        size_t dap_ptr = num_dap_temp_data - num_docs;

        for (size_t i = num_lcp_temp_data; i > 0; i--) {
            // Re-initialize at each position
            std::fill(curr_da_profile.begin(), curr_da_profile.end(), 0);

            // Grab the data for current suffix
            bool is_start = GET_IS_START(mmap_lcp_inter, (i-1));
            bool is_end = GET_IS_END(mmap_lcp_inter, (i-1));
            uint8_t bwt_ch = GET_BWT_CH(mmap_lcp_inter, (i-1));
            size_t doc_of_LF_i = GET_DOC_OF_LF(mmap_lcp_inter, (i-1));

            // If suffix is a run boundary, update its profile if necessary
            if (is_start || is_end) {
                initialize_current_row_profile(doc_of_LF_i, curr_da_profile, bwt_ch);
                for (size_t j = 0; j < num_docs; j++) {
                    if (GET_DAP(mmap_dap_inter, (dap_ptr+j)) < curr_da_profile[j]){
                        mmap_dap_inter[(dap_ptr+j) * DOCWIDTH] = (0xFF & curr_da_profile[j]);
                        mmap_dap_inter[(dap_ptr+j) * DOCWIDTH + 1] = ((0xFF << 8) & curr_da_profile[j]) >> 8;
                    }
                }
                dap_ptr -= num_docs;
                assert(dap_ptr >= 0);
            }
            ch_doc_encountered[bwt_ch][doc_of_LF_i] = true;

            // Update the predecessor table for next suffix
            size_t lcp_i = GET_LCP(mmap_lcp_inter, (i-1));
            update_predecessor_max_lcp_table_up(lcp_i, doc_of_LF_i, bwt_ch);
        }

        // print the document array profiles
        print_dap(num_lcp_temp_data);

        // print last BWT char and SA sample
        print_sa();
        print_bwt();
        total_num_runs = curr_run_num;

        // close output files
        fclose(ssa_file); fclose(esa_file);
        fclose(bwt_file);
        fclose(lcp_file);
        fclose(sdap_file); fclose(edap_file);

        // delete temporary files
        delete_temp_files(temp_prefix);

        // print out build time
        DONE_LOG((std::chrono::system_clock::now() - start));
    }

    ~pfp_lcp_doc_two_pass() {
        // Deallocate memory for predecessor table
        for (size_t i = 0; i < 256; i++)
            delete[] predecessor_max_lcp[i];
        delete[] predecessor_max_lcp;

        // Unmap the temp data file
        munmap(mmap_lcp_inter, tmp_file_size);
        munmap(mmap_dap_inter, tmp_file_size);
        close(dap_inter_fd); close(lcp_inter_fd);
    }

    private:
        typedef struct
        {
            size_t i = 0; // This should be safe since the first entry of sa is always the dollarsign used to compute the sa
            size_t phrase = 0;
            size_t suffix_length = 0;
            int_da sn = 0;
            uint8_t bwt_char = 0;
        } phrase_suffix_t;

        FILE *lcp_file; // LCP array
        FILE *bwt_file; // BWT (run characters if using rle)
        FILE *bwt_file_len; // lengths file is using rle
        FILE *ssa_file; // start of suffix array sample
        FILE *esa_file; // end of suffix array sample
        FILE *sdap_file; // start of document array profiles
        FILE *edap_file; // end of document array profiles

        size_t num_docs = 0;
        size_t j = 0;
        size_t ssa = 0;
        size_t esa = 0;
        size_t num_blocks_of_32 = 0;
        uint8_t curr_bwt_ch = 0;

        size_t num_dap_temp_data = 0;
        size_t num_lcp_temp_data = 0;

        // matrix of <ch, doc> pairs that keep track of which pairs 
        // we have seen so far. This is used when we initialize
        // the curr_da_profile with the max lcps with previous lcps
        std::vector<std::vector<bool>> ch_doc_encountered;

        // Data-structure to store the max LCP with respect to all previous <ch, doc> pairs
        uint16_t** predecessor_max_lcp;

        // This structure contains the raw document array profile values. The 
        // ith continguous group of num_doc integers is a profile for lcp_queue[i]
        std::deque<size_t> lcp_queue_profiles;

        // Variables are used for the intermediate file
        int dap_inter_fd, lcp_inter_fd;
        struct stat file_info_dap_inter;
        struct stat file_info_lcp_inter;
        char* mmap_dap_inter;
        char* mmap_lcp_inter;

        /*
         * These methods are focused on the document array construction
         * portions of code.
         */
        void initialize_index_files(std::string filename, std::string temp_prefix) {
            /* opening output files for data-structures like  LCP, SA, BWT */

            // LCP data-structure
            std::string outfile = filename + std::string(".lcp");
            if ((lcp_file = fopen(outfile.c_str(), "w")) == nullptr)
                error("open() file " + outfile + " failed");
            
            // Suffix array samples at start of runs
            outfile = filename + std::string(".ssa");
            if ((ssa_file = fopen(outfile.c_str(), "w")) == nullptr)
                error("open() file " + outfile + " failed");
            
            // Suffix array samples at end of runs
            outfile = filename + std::string(".esa");
            if ((esa_file = fopen(outfile.c_str(), "w")) == nullptr)
                error("open() file " + outfile + " failed");
            
            // BWT index files
            if (rle) {
                outfile = filename + std::string(".bwt.heads");
                if ((bwt_file = fopen(outfile.c_str(), "w")) == nullptr)
                    error("open() file " + outfile + " failed");

                outfile = filename + std::string(".bwt.len");
                if ((bwt_file_len = fopen(outfile.c_str(), "w")) == nullptr)
                    error("open() file " + outfile + " failed");
            } else {
                outfile = filename + std::string(".bwt");
                if ((bwt_file = fopen(outfile.c_str(), "w")) == nullptr)
                    error("open() file " + outfile + " failed");
            }   

            outfile = filename + std::string(".sdap");
            if ((sdap_file = fopen(outfile.c_str(), "w")) == nullptr) 
                error("open() file " + outfile + " failed");
            if (fwrite(&num_docs, sizeof(size_t), 1, sdap_file) != 1)
                error("SDAP write error: number of documents");

            outfile = filename + std::string(".edap");
            if ((edap_file = fopen(outfile.c_str(), "w")) == nullptr)
                error("open() file " + outfile + " failed");
            if (fwrite(&num_docs, sizeof(size_t), 1, edap_file) != 1)
                error("SDAP write error: number of documents"); 

            // Open intermediate files for storing data
            mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

            // 1. File for LCP Queue data
            outfile = temp_prefix + std::string(".tmp_lcp_data");
            lcp_inter_fd = open(outfile.c_str(), O_RDWR | O_CREAT | O_TRUNC, mode);
            ftruncate(lcp_inter_fd, tmp_file_size);

            if (lcp_inter_fd == -1)
                FATAL_ERROR("Issue occurred when opening file for intermediate file.");
            if (fstat(lcp_inter_fd, &file_info_lcp_inter) == -1)
                FATAL_ERROR("Issue with checking file size on overflow file.");

            mmap_lcp_inter = (char*) mmap(NULL, 
                                         tmp_file_size, 
                                         PROT_READ | PROT_WRITE, 
                                         MAP_SHARED, 
                                         lcp_inter_fd, 0);
            assert(mmap_lcp_inter != MAP_FAILED);
            
            // 2. File for Document Array Profiles
            outfile = temp_prefix + std::string(".tmp_dap_data");
            dap_inter_fd = open(outfile.c_str(), O_RDWR | O_CREAT | O_TRUNC, mode);
            ftruncate(dap_inter_fd, tmp_file_size);

            if (dap_inter_fd == -1)
                FATAL_ERROR("Issue occurred when opening file for intermediate file.");
            if (fstat(dap_inter_fd, &file_info_dap_inter) == -1)
                FATAL_ERROR("Issue with checking file size on overflow file.");

            mmap_dap_inter = (char*) mmap(NULL, 
                                         tmp_file_size, 
                                         PROT_READ | PROT_WRITE, 
                                         MAP_SHARED, 
                                         dap_inter_fd, 0);
            assert(mmap_dap_inter != MAP_FAILED);
        }

        void initialize_tmp_size_variables() {
            /* determine how many records we can store in each temporary file */
            max_lcp_records = tmp_file_size/TEMPDATA_RECORD; 
            max_dap_records = tmp_file_size/DOCWIDTH;
        }

        void update_predecessor_max_lcp_table(size_t lcp_i, size_t total_length, size_t pos_of_LF_i, size_t doc_of_LF_i, uint8_t bwt_ch) {
            // Update the predecessor lcp table, this allows us to compute the maximum
            // lcp with respect to all the predecessor occurrences of other documents.
            // For example: if we are at <A, 2>, we will find the maximum lcp with the 
            // the previous occurrences of <A, 0> and <A, 1> for 3 documents.
            #if AVX512BW_PRESENT       
                /*
                * NOTE: I decided to use the mask store/load because the non-mask
                * version of the same function was not present. I looked online and 
                * found this thread, that describes how you can replicate their function
                * by using masks which is what I did: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95483
                * This link was helpful in understanding conditional
                * load masks: https://www.cs.virginia.edu/~cr4bd/3330/F2018/simdref.html
                */

                // initialize an lcp_i vector
                uint16_t lcp_i_vector[32];
                for (size_t i = 0; i < 32; i++)
                    lcp_i_vector[i] = std::min((size_t) MAXLCPVALUE, lcp_i);

                // load the array of constants (lcp_i)
                __m512i arr1, arr2, arr3; 
                __mmask32 k = ~0; // all 32 bits on, means all 32 values will be written
                arr2 = _mm512_maskz_loadu_epi16(~0, (const __m512i*) &lcp_i_vector[0]);

                //std::vector<size_t> dna_chars = {65, 67, 71, 84, 85}; // A, C, G, T, U
                //for (size_t ch_num: dna_chars) { // Optimization for DNA
                for (size_t ch_num = 0; ch_num < 256; ch_num++) {
                    // use SIMD for all groups of 32
                    for (size_t i = 0; i < (num_blocks_of_32 * 32); i+=32) {
                        // zero-mask, all the set bit positions are loaded
                        arr1 = _mm512_maskz_loadu_epi16(~0, (const __m512i*) &predecessor_max_lcp[ch_num][i]); 

                        arr3 = _mm512_min_epu16(arr1, arr2);
                        _mm512_mask_storeu_epi16((__m512i*) &predecessor_max_lcp[ch_num][i], k, arr3); 
                    }
                }
                // Reset the LCP with respect to the current <ch, doc> pair
                predecessor_max_lcp[bwt_ch][doc_of_LF_i] = std::min((size_t) MAXLCPVALUE, total_length - pos_of_LF_i);
            
            #else                        
                for (size_t ch_num = 0; ch_num < 256; ch_num++) {
                    for (size_t doc_num = 0; doc_num < num_docs; doc_num++) {
                        predecessor_max_lcp[ch_num][doc_num] = std::min(predecessor_max_lcp[ch_num][doc_num], (uint16_t) std::min(lcp_i, (size_t) MAXLCPVALUE));
                    }
                }
                // Reset the LCP with respect to the current <ch, doc> pair
                predecessor_max_lcp[bwt_ch][doc_of_LF_i] = std::min((size_t) MAXLCPVALUE, total_length - pos_of_LF_i);
            #endif
        }

        void update_predecessor_max_lcp_table_up(size_t lcp_i, size_t doc_of_LF_i, uint8_t bwt_ch) {
            // Update the predecessor lcp table, this allows us to compute the maximum
            // lcp with respect to all the predecessor occurrences of other documents.
            // For example: if we are at <A, 2>, we will find the maximum lcp with the 
            // the previous occurrences of <A, 0> and <A, 1> for 3 documents.
            #if AVX512BW_PRESENT       
                /*
                * NOTE: I decided to use the mask store/load because the non-mask
                * version of the same function was not present. I looked online and 
                * found this thread, that describes how you can replicate their function
                * by using masks which is what I did: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95483
                * This link was helpful in understanding conditional
                * load masks: https://www.cs.virginia.edu/~cr4bd/3330/F2018/simdref.html
                */

                // initialize an lcp_i vector
                uint16_t lcp_i_vector[32];
                for (size_t i = 0; i < 32; i++)
                    lcp_i_vector[i] = std::min((size_t) MAXLCPVALUE, lcp_i);

                // load the array of constants (lcp_i)
                __m512i arr1, arr2, arr3; 
                __mmask32 k = ~0; // all 32 bits on, means all 32 values will be written
                arr2 = _mm512_maskz_loadu_epi16(~0, (const __m512i*) &lcp_i_vector[0]);

                // Reset the LCP with respect to the current <ch, doc> pair
                predecessor_max_lcp[bwt_ch][doc_of_LF_i] = std::min((size_t) MAXLCPVALUE, lcp_i);

                //std::vector<size_t> dna_chars = {65, 67, 71, 84, 85}; // A, C, G, T, U
                //for (size_t ch_num: dna_chars) { // Optimization for DNA
                for (size_t ch_num = 0; ch_num < 256; ch_num++) {
                    // use SIMD for all groups of 32
                    for (size_t i = 0; i < (num_blocks_of_32 * 32); i+=32) {
                        // zero-mask, all the set bit positions are loaded
                        arr1 = _mm512_maskz_loadu_epi16(~0, (const __m512i*) &predecessor_max_lcp[ch_num][i]); 

                        arr3 = _mm512_min_epu16(arr1, arr2);
                        _mm512_mask_storeu_epi16((__m512i*) &predecessor_max_lcp[ch_num][i], k, arr3); 
                    }
                }            
            #else        
                // Reset the LCP with respect to the current <ch, doc> pair
                predecessor_max_lcp[bwt_ch][doc_of_LF_i] = std::min((size_t) MAXLCPVALUE, lcp_i);

                for (size_t ch_num = 0; ch_num < 256; ch_num++) {
                    for (size_t doc_num = 0; doc_num < num_docs; doc_num++) {
                        predecessor_max_lcp[ch_num][doc_num] = std::min(predecessor_max_lcp[ch_num][doc_num], (uint16_t) std::min(lcp_i, (size_t) MAXLCPVALUE));
                    }
                }
            #endif    
        }

        void initialize_current_row_profile(size_t doc_of_LF_i, std::vector<size_t>& curr_da_profile, uint8_t bwt_ch){
            // Initialize the curr_da_profile with max lcp for predecessor 
            // occurrences of the same BWT character from another document, and
            // we check and make sure they occurred to avoid initializing it
            // with 1 (0 + 1 = 1)
            for (size_t i = 0; i < num_docs; i++) {
                if (i != doc_of_LF_i && ch_doc_encountered[bwt_ch][i])
                    curr_da_profile[i] = predecessor_max_lcp[bwt_ch][i] + 1;
            }
        }

        void write_data_to_temp_file(temp_data_entry_t data_entry) {
            /* Write to temp lcp queue data to the file */
            size_t start_pos = num_lcp_temp_data * TEMPDATA_RECORD;
            mmap_lcp_inter[start_pos] = 0x00 | (data_entry.is_start << 1)  | (data_entry.is_end);
            mmap_lcp_inter[start_pos + 1] = data_entry.bwt_ch;
            
            // little-endian orientation
            mmap_lcp_inter[start_pos + 2] = (data_entry.doc_num & 0xFF);
            mmap_lcp_inter[start_pos + 3] = (data_entry.doc_num & (0xFF << 8)) >> 8;

            mmap_lcp_inter[start_pos + 4] = (data_entry.lcp_i & 0xFF);
            mmap_lcp_inter[start_pos + 5] = (data_entry.lcp_i & (0xFF << 8)) >> 8;
            
            num_lcp_temp_data += 1;
            assert(max_lcp_records >= num_lcp_temp_data);
        }

        void write_profile_to_temp_file(std::vector<size_t>& curr_prof) {
            /* write the current profile to the temporary storage */
            size_t start_pos = num_dap_temp_data * DOCWIDTH;
            for (size_t i = 0; i < curr_prof.size(); i++) {
                uint16_t curr_lcp = std::min((size_t) MAXLCPVALUE, curr_prof[i]);
                mmap_dap_inter[start_pos + (i*DOCWIDTH)] = (0xFF & curr_lcp);
                mmap_dap_inter[start_pos + (i*DOCWIDTH) + 1] = ((0xFF << 8) & curr_lcp) >> 8;
            }
            num_dap_temp_data += curr_prof.size();
            assert(max_dap_records >= (num_dap_temp_data+num_docs));
        }

        void delete_temp_files(std::string filename) {
            /* delete the temporary files */
            if (std::remove((filename + ".tmp_dap_data").data()))
                FATAL_ERROR("issue occurred while deleting temporary file."); 
            if (std::remove((filename + ".tmp_lcp_data").data()))
                FATAL_ERROR("issue occurred while deleting temporary file."); 
        }

        void print_dap(size_t num_entries) {
            /* writes the document-array profiles to file */
            size_t dap_ptr = 0;
            for (size_t i = 0; i < num_entries; i++) {
                // Grab the data for current suffix
                bool is_start = GET_IS_START(mmap_lcp_inter, i);
                bool is_end = GET_IS_END(mmap_lcp_inter, i);
                uint8_t bwt_ch = GET_BWT_CH(mmap_lcp_inter, i);
                size_t doc_of_LF_i = GET_DOC_OF_LF(mmap_lcp_inter, i);

                // Load profiles if its a run-boundary
                std::vector<size_t> curr_prof (num_docs, 0);
                if (is_start || is_end) {
                    // Write the BWT character
                    if (is_start && fwrite(&bwt_ch, 1, 1, sdap_file) != 1)
                        FATAL_ERROR("issue occurred while writing to *.sdap file");
                    if (is_end && fwrite(&bwt_ch, 1, 1, edap_file) != 1)
                        FATAL_ERROR("issue occurred while writing to *.sdap file");
                    
                    // Write the profile
                    for (size_t j = 0; j < num_docs; j++) {
                        size_t prof_val = GET_DAP(mmap_dap_inter, (dap_ptr+j));
                        assert(prof_val <= MAXLCPVALUE);

                        if (is_start && fwrite(&prof_val, DOCWIDTH, 1, sdap_file) != 1)
                            FATAL_ERROR("issue occurred while writing to *.sdap file");
                        if (is_end && fwrite(&prof_val, DOCWIDTH, 1, edap_file) != 1)
                            FATAL_ERROR("issue occurred while writing to *.edap file");
                    }
                    dap_ptr += num_docs;
                }
            }
        }

        /* 
         * These methods are focused on the non-document array 
         * portions of the constructions like the BWT, LCP, and
         * Suffix Array.
         */

        inline bool inc(phrase_suffix_t& s){
            s.i++;
            if (s.i >= pf.dict.saD.size())
                return false;
            s.sn = pf.dict.saD[s.i];
            s.phrase = pf.dict.rank_b_d(s.sn);
            // s.phrase = pf.dict.daD[s.i] + 1; // + 1 because daD is 0-based
            assert(!is_valid(s) || (s.phrase > 0 && s.phrase < pf.pars.ilist.size()));
            s.suffix_length = pf.dict.select_b_d(pf.dict.rank_b_d(s.sn + 1) + 1) - s.sn - 1;
            if(is_valid(s))
                s.bwt_char = (s.sn == pf.w ? 0 : pf.dict.d[s.sn - 1]);
            return true;
        }

        inline bool is_valid(phrase_suffix_t& s){
            // avoid the extra w # at the beginning of the text
            if (s.sn < pf.w)
                return false;
            // Check if the suffix has length at least w and is not the complete phrase.
            if (pf.dict.b_d[s.sn] != 0 || s.suffix_length < pf.w)
                return false;
            
            return true;
        }
        
        inline int_t min_s_lcp_T(size_t left, size_t right){
            // assume left < right
            if (left > right)
                std::swap(left, right);

            assert(pf.s_lcp_T[pf.rmq_s_lcp_T(left + 1, right)] >= pf.w);

            return (pf.s_lcp_T[pf.rmq_s_lcp_T(left + 1, right)] - pf.w);
        }

        inline int_t compute_lcp_suffix(phrase_suffix_t& curr, phrase_suffix_t& prev){
            int_t lcp_suffix = 0;

            if (j > 0)
            {
                // Compute phrase boundary lcp
                lcp_suffix = pf.dict.lcpD[curr.i];
                for (size_t k = prev.i + 1; k < curr.i; ++k)
                {
                    lcp_suffix = std::min(lcp_suffix, pf.dict.lcpD[k]);
                }

                if (lcp_suffix >= curr.suffix_length && curr.suffix_length == prev.suffix_length)
                {
                    // Compute the minimum s_lcpP of the phrases following the two phrases
                    // we take the first occurrence of the phrase in BWT_P
                    size_t left = pf.pars.ilist[pf.pars.select_ilist_s(curr.phrase + 1)]; //size_t left = first_P_BWT_P[phrase];
                    // and the last occurrence of the previous phrase in BWT_P
                    size_t right = pf.pars.ilist[pf.pars.select_ilist_s(prev.phrase + 2) - 1]; //last_P_BWT_P[prev_phrase];
                    
                    lcp_suffix += min_s_lcp_T(left,right);
                }
            }

            return lcp_suffix;
        }

        inline void print_lcp(int_t val, size_t pos){
            size_t tmp_val = val;
            if (fwrite(&tmp_val, THRBYTES, 1, lcp_file) != 1)
                error("LCP write error 1");
        }

        inline void new_min_s(int_t val, size_t pos){
            // We can put here the check if we want to store the LCP or stream it out
            min_s.push_back(val);
            pos_s.push_back(j);
        }

        inline void update_ssa(phrase_suffix_t &curr, size_t pos){
            ssa = (pf.pos_T[pos] - curr.suffix_length) % (pf.n - pf.w + 1ULL); // + pf.w;
            assert(ssa < (pf.n - pf.w + 1ULL));
        }

        inline void update_esa(phrase_suffix_t &curr, size_t pos){
            esa = (pf.pos_T[pos] - curr.suffix_length) % (pf.n - pf.w + 1ULL); // + pf.w;
            assert(esa < (pf.n - pf.w + 1ULL));
        }

        inline void print_sa(){
            if (j < (pf.n - pf.w + 1ULL))
            {
                size_t pos = j;
                if (fwrite(&pos, SSABYTES, 1, ssa_file) != 1)
                    error("SA write error 1");
                if (fwrite(&ssa, SSABYTES, 1, ssa_file) != 1)
                    error("SA write error 2");
            }

            if (j > 0)
            {
                size_t pos = j - 1;
                if (fwrite(&pos, SSABYTES, 1, esa_file) != 1)
                    error("SA write error 1");
                if (fwrite(&esa, SSABYTES, 1, esa_file) != 1)
                    error("SA write error 2");
            }
        }

        inline void print_bwt(){   
            if (length > 0)
            {
                if (rle) {
                    // write the head character
                    if (fputc(head, bwt_file) == EOF)
                        error("BWT write error 1");

                    // write the length of that run
                    if (fwrite(&length, BWTBYTES, 1, bwt_file_len) != 1)
                        error("BWT write error 2");
                } else {
                    for (size_t i = 0; i < length; ++i)
                    {
                        if (fputc(head, bwt_file) == EOF)
                            error("BWT write error 1");
                    }
                }
            }
        }

        inline void update_bwt(uint8_t next_char, size_t length_){
            if (head != next_char)
            {
                print_sa();
                print_bwt();

                head = next_char;

                length = 0;
            }
            length += length_;

        }
};

#endif /* end of include guard: _LCP_PFP_HH */