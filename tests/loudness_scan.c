/*
* Copyright (c) 2025 Odalith
 *
 * ffaudio is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffaudio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFaudio; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Measure the loudness of files (read only, nothing is written).
 *
 *   loudness_scan [-t] [-c] <file>...
 *
 * The files given together are treated as one album. -t skips the true peak (faster),
 * -c prints CSV: file, integrated LUFS, LRA, true peak, sample peak, ReplayGain dB, R128 Q7.8.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../lib/ffaudio_loudness.h"

static double db(const double x) { return x > 0 ? 20.0 * log10(x) : -INFINITY; }

static void print_row(const bool csv, const char *name, const AuLoudness *l) {
    if (csv) {
        printf("\"%s\",%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", name, l->integrated_lufs, l->loudness_range_lu,
               db(l->true_peak), db(l->sample_peak), l->replaygain_gain_db, l->r128_gain_q78);
    }
    else if (l->silent) {
        printf("%-9s %s\n", "silent", name);
    }
    else {
        printf("%7.2f %6.2f %7.2f %7.2f %+7.2f %7d  %s\n", l->integrated_lufs, l->loudness_range_lu,
               db(l->true_peak), db(l->sample_peak), l->replaygain_gain_db, l->r128_gain_q78, name);
    }
}

int main(int argc, char **argv) {
    AuLoudnessOptions config = {0};
    bool csv = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (!strcmp(argv[first], "-t")) config.skip_true_peak = true;
        else if (!strcmp(argv[first], "-c")) csv = true;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: %s [-t] [-c] <file>...\n", argv[0]);
        return 1;
    }

    AuLoudnessMeter *album = NULL;   // every file measured, merged

    if (csv) printf("file,integrated_lufs,lra_lu,true_peak_dbtp,sample_peak_dbfs,replaygain_db,r128_q78\n");
    else printf("   LUFS     LRA    dBTP    dBFS      RG    R128  file\n");

    int failed = 0;
    for (int i = first; i < argc; i++) {
        AuLoudness l;
        AuLoudnessMeter *m = NULL;
        const int ret = au_loudness_measure(argv[i], &config, &m);
        if (ret != AU_OK) {
            fprintf(stderr, "%s: could not be measured (%d)\n", argv[i], ret);
            failed++;
            continue;
        }
        au_loudness_result(m, &l);
        print_row(csv, argv[i], &l);
        if (!album) album = m;
        else {
            au_loudness_merge(album, m);
            au_loudness_meter_free(m);
        }
    }

    AuLoudness values;
    if (au_loudness_file_count(album) > 1 && au_loudness_result(album, &values) == AU_OK)
        print_row(csv, "(album)", &values);

    au_loudness_meter_free(album);
    return failed ? 2 : 0;
}
