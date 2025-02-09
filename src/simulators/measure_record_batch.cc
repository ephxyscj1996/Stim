/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "measure_record_batch.h"

#include <algorithm>

#include "measure_record_batch_writer.h"

using namespace stim_internal;

MeasureRecordBatch::MeasureRecordBatch(size_t num_shots, size_t max_lookback)
    : max_lookback(max_lookback), unwritten(0), stored(0), written(0), shot_mask(num_shots), storage(1, num_shots) {
    for (size_t k = 0; k < num_shots; k++) {
        shot_mask[k] = true;
    }
}

void MeasureRecordBatch::record_result(simd_bits_range_ref result) {
    if (stored >= storage.num_major_bits_padded()) {
        simd_bit_table new_storage(storage.num_major_bits_padded() * 2, storage.num_minor_bits_padded());
        new_storage.data.word_range_ref(0, storage.data.num_simd_words) = storage.data;
        storage = std::move(new_storage);
    }
    storage[stored] = result;
    storage[stored] &= shot_mask;
    stored++;
    unwritten++;
}

simd_bits_range_ref MeasureRecordBatch::lookback(size_t lookback) const {
    if (lookback > stored) {
        throw std::out_of_range("Referred to a measurement record before the beginning of time.");
    }
    if (lookback == 0) {
        throw std::out_of_range("Lookback must be non-zero.");
    }
    if (lookback > max_lookback) {
        throw std::out_of_range("Referred to a measurement record past the lookback limit.");
    }
    return storage[stored - lookback];
}

void MeasureRecordBatch::mark_all_as_written() {
    unwritten = 0;
    size_t m = max_lookback;
    if ((stored >> 1) > m) {
        memcpy(storage.data.u8, storage[stored - m].u8, m * storage.num_minor_u8_padded());
        stored = m;
    }
}

void MeasureRecordBatch::intermediate_write_unwritten_results_to(
    MeasureRecordBatchWriter &writer, simd_bits_range_ref ref_sample) {
    while (unwritten >= 1024) {
        auto slice = storage.slice_maj(stored - unwritten, stored - unwritten + 1024);
        for (size_t k = 0; k < 1024; k++) {
            size_t j = written + k;
            if (j < ref_sample.num_bits_padded() && ref_sample[j]) {
                slice[k] ^= shot_mask;
            }
        }
        writer.batch_write_bytes(slice, 1024 >> 6);
        unwritten -= 1024;
        written += 1024;
    }

    size_t m = std::max(max_lookback, unwritten);
    if ((stored >> 1) > m) {
        memcpy(storage.data.u8, storage[stored - m].u8, m * storage.num_minor_u8_padded());
        stored = m;
    }
}

void MeasureRecordBatch::final_write_unwritten_results_to(
    MeasureRecordBatchWriter &writer, simd_bits_range_ref ref_sample) {
    size_t n = stored;
    for (size_t k = n - unwritten; k < n; k++) {
        bool invert = written < ref_sample.num_bits_padded() && ref_sample[written];
        if (invert) {
            storage[k] ^= shot_mask;
        }
        writer.batch_write_bit(storage[k]);
        if (invert) {
            storage[k] ^= shot_mask;
        }
        written++;
    }
    unwritten = 0;
    writer.write_end();
}

void MeasureRecordBatch::clear() {
    stored = 0;
    unwritten = 0;
}
