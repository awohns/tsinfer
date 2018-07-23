/*
** Copyright (C) 2018 University of Oxford
**
** This file is part of tsinfer.
**
** tsinfer is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** tsinfer is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with tsinfer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "tsinfer.h"
#include "err.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "avl.h"

typedef struct {
    allele_t *state;
    site_id_t id;
    size_t num_samples;
} site_equality_t;


static int
cmp_pattern_map(const void *a, const void *b) {
    const pattern_map_t *ia = (pattern_map_t const *) a;
    const pattern_map_t *ib = (pattern_map_t const *) b;
    int ret = memcmp(ia->genotypes, ib->genotypes, ia->num_samples * sizeof(allele_t));
    return ret;
}

int
ancestor_builder_alloc(ancestor_builder_t *self, size_t num_samples, size_t num_sites,
        int flags)
{
    int ret = 0;
    size_t j;
    // TODO error checking
    //
    assert(num_samples > 1);
    /* TODO need to be able to handle zero sites */
    /* assert(num_sites > 0); */

    memset(self, 0, sizeof(ancestor_builder_t));
    self->num_samples = num_samples;
    self->num_sites = num_sites;
    self->flags = flags;
    self->sites = calloc(num_sites, sizeof(site_t));
    self->frequency_map = calloc(num_samples + 1, sizeof(avl_tree_t));
    self->descriptors = calloc(num_sites, sizeof(ancestor_descriptor_t));
    if (self->sites == NULL || self->frequency_map == NULL
            || self->descriptors == NULL) {
        ret = TSI_ERR_NO_MEMORY;
        goto out;
    }
    ret = block_allocator_alloc(&self->allocator, 1024 * 1024);
    if (ret != 0) {
        goto out;
    }
    for (j = 0; j < num_samples + 1; j++) {
        avl_init_tree(&self->frequency_map[j], cmp_pattern_map, NULL);
    }
out:
    return ret;
}

int
ancestor_builder_free(ancestor_builder_t *self)
{
    tsi_safe_free(self->sites);
    tsi_safe_free(self->frequency_map);
    tsi_safe_free(self->descriptors);
    block_allocator_free(&self->allocator);
    return 0;
}

static inline void
ancestor_builder_get_consistent_samples(ancestor_builder_t *self, site_id_t site,
        node_id_t *samples, size_t *num_samples)
{
    node_id_t j, k;
    allele_t *restrict genotypes = self->sites[site].genotypes;

    k = 0;
    for (j = 0; j < (node_id_t) self->num_samples; j++) {
        if (genotypes[j] == 1) {
            samples[k] = j;
            k++;
        }
    }
    *num_samples = (size_t) k;
}

#if 0
static inline bool
ancestor_builder_make_site(ancestor_builder_t *self, site_id_t focal_site_id,
        site_id_t site_id, node_id_t *restrict consistent_samples,
        size_t *num_consistent_samples, allele_t *ancestor)
{
    size_t j, k, l, ones, zeros;
    const site_t focal_site = self->sites[focal_site_id];
    allele_t *restrict site_genotypes = self->sites[site_id].genotypes;
    allele_t consensus;
    size_t current_num_samples = *num_consistent_samples;
    size_t min_sample_set_size = focal_site.frequency / 2;

    if (self->sites[site_id].frequency > focal_site.frequency) {
        ones = 0;
        for (j = 0; j < current_num_samples; j++) {
            ones += site_genotypes[consistent_samples[j]];
        }
        zeros = current_num_samples - ones;
        if (ones > zeros) {
            consensus = 1;
        } else if (ones < zeros) {
            consensus = 0;
        } else {
            /* Equal numbers so we don't know what to do */
            return false;
        }

        /* Go through the consistent samples and filter down to those
         * that agree with the consensus */
        k = 0;
        for (j = 0; j < current_num_samples; j++) {
            l = consistent_samples[j];
            if (site_genotypes[l] == consensus) {
                consistent_samples[k] = l;
                k++;
            }
        }
        /* if (k == 1) { */
        if (k < min_sample_set_size) {
            /* printf("Breaking k = %d, %d\n", k, (int) min_sample_set_size); */
            return false;
        }
        *num_consistent_samples = k;
        ancestor[site_id] = consensus;
        /* printf("%d\tconsensus=%d\tin_k=%d\tout_k=%d\n", */
        /*         (int) site_id, consensus, (int) current_num_samples, */
        /*         (int) k); */
    } else {
        ancestor[site_id] = 0;
    }
    return true;
}

/* Build the ancestors for sites in the specified focal sites */
int
ancestor_builder_make_ancestor(ancestor_builder_t *self, size_t num_focal_sites,
        site_id_t *focal_sites, site_id_t *ret_start, site_id_t *ret_end,
        allele_t *ancestor)
{
    int ret = 0;
    int64_t l;
    site_id_t j, k, focal_site, start, end;
    size_t num_consistent_samples = 0;
    size_t num_sites = self->num_sites;
    node_id_t *consistent_samples = malloc(self->num_samples * sizeof(node_id_t));
    bool consistent;

    if (consistent_samples == NULL) {
        goto out;
    }
    // TODO proper error checking.
    assert(num_focal_sites > 0);

    ancestor_builder_get_consistent_samples(self, focal_sites[0], consistent_samples,
            &num_consistent_samples);
    assert(num_consistent_samples == self->sites[focal_sites[0]].frequency);

    /* Set any unknown values to -1 */
    memset(ancestor, 0xff, num_sites * sizeof(allele_t));

    /* Fill in the sites within the bounds of the focal sites. We have previously
     * checked that there is no disagreement among the consistent_samples, so we
     * just take the value from one of the samples */
    focal_site = focal_sites[0];
    ancestor[focal_site] = 1;
    for (j = 1; j < (site_id_t) num_focal_sites; j++) {
        for (k = focal_sites[j - 1] + 1; k < focal_sites[j]; k++) {
            ancestor[k] = 0;
            if (self->sites[k].frequency > self->sites[focal_site].frequency) {
                ancestor[k] = self->sites[k].genotypes[consistent_samples[0]];
            }
        }
        ancestor[focal_sites[j]] = 1;
    }
    /* Work leftwards from the first focal site */
    focal_site = focal_sites[0];
    consistent = true;
    for (l = ((int64_t) focal_site) - 1; l >= 0 && consistent; l--) {
        consistent = ancestor_builder_make_site(self, focal_site, l,
                consistent_samples, &num_consistent_samples, ancestor);
    }
    start = l + 1 + (int) !consistent;

    ancestor_builder_get_consistent_samples(self, focal_sites[0], consistent_samples,
            &num_consistent_samples);
    assert(num_consistent_samples == self->sites[focal_sites[0]].frequency);
    /* Work rightwards from the last focal site */
    consistent = true;
    focal_site = focal_sites[num_focal_sites - 1];
    for (l = focal_site + 1; l < (int64_t) num_sites && consistent; l++) {
        consistent = ancestor_builder_make_site(self, focal_site, l,
                consistent_samples, &num_consistent_samples, ancestor);
    }
    end = l - (int) !consistent;

    *ret_start = start;
    *ret_end = end;
out:
    tsi_safe_free(consistent_samples);
    return ret;
}
#endif

static int
ancestor_builder_compute_older_sites(ancestor_builder_t *self,
        site_id_t focal_site, allele_t *ancestor,
        site_id_t *older_sites, size_t num_older_sites,
        node_id_t *sample_set, size_t sample_set_size,
        site_id_t *last_site_ret)
{
    int ret = 0;
    site_id_t last_site = focal_site;
    site_id_t l;
    node_id_t u;
    size_t j, k, ones, zeros, tmp_size;
    size_t min_sample_set_size = sample_set_size / 2;
    bool *restrict disagree = calloc(self->num_samples, sizeof(*disagree));
    allele_t *restrict genotypes;
    allele_t consensus;

    if (disagree == NULL) {
        ret = TSI_ERR_NO_MEMORY;
        goto out;
    }
    /* printf("site=%d, older_sites=%d\n", (int) focal_site, (int) num_older_sites); */
    for (j = 0; j < num_older_sites; j++) {
        l = older_sites[j];
        /* printf("\t%d\t%d:", l, (int) sample_set_size); */
        /* for (k = 0; k < sample_set_size; k++) { */
        /*     printf("%d, ", sample_set[k]); */
        /* } */

        genotypes = self->sites[l].genotypes;
        ones = 0;
        for (k = 0; k < sample_set_size; k++) {
            ones += genotypes[sample_set[k]];
        }
        zeros = sample_set_size - ones;
        consensus = 0;
        if (ones >= zeros) {
            consensus = 1;
        }
        /* printf("\t:ones=%d, consensus=%d\n", (int) ones, consensus); */
        for (k = 0; k < sample_set_size; k++) {
            u = sample_set[k];
            if (disagree[u] && genotypes[u] != consensus) {
                /* This sample has disagreed with consensus twice in a row,
                 * so remove it */
                /* printf("\t\tremoving %d\n", sample_set[k]); */
                sample_set[k] = -1;
            }
        }
        /* Repack the sample set */
        tmp_size = 0;
        for (k = 0; k < sample_set_size; k++) {
            if (sample_set[k] != -1) {
                sample_set[tmp_size] = sample_set[k];
                tmp_size++;
            }
        }
        sample_set_size = tmp_size;
        if (sample_set_size <= min_sample_set_size) {
            /* printf("BREAK\n"); */
            break;
        }
        ancestor[l] = consensus;
        last_site = l;
        /* For the remaining sample set, set the disagree flags based
         * on whether they agree with the consensus for this site. */
        for (k = 0; k < sample_set_size; k++) {
            u = sample_set[k];
            disagree[u] = genotypes[u] != consensus;
        }
    }
    *last_site_ret = last_site;
out:
    tsi_safe_free(disagree);
    return ret;
}

/* Build the ancestors for sites in the specified focal sites */
int
ancestor_builder_make_ancestor(ancestor_builder_t *self, size_t num_focal_sites,
        site_id_t *focal_sites, site_id_t *ret_start, site_id_t *ret_end,
        allele_t *ancestor)
{
    int ret = 0;
    int64_t l;
    site_id_t focal_site, last_site;
    size_t focal_site_frequency, num_older_sites, sample_set_size;
    site_id_t *older_sites = malloc(self->num_sites * sizeof(*older_sites));
    node_id_t *sample_set = malloc(self->num_samples * sizeof(node_id_t));

    if (older_sites == NULL || sample_set == NULL) {
        ret = TSI_ERR_NO_MEMORY;
        goto out;
    }
    assert(num_focal_sites == 1);

    focal_site = focal_sites[0];
    focal_site_frequency = self->sites[focal_site].frequency;
    memset(ancestor, 0xff, self->num_sites * sizeof(*ancestor));
    ancestor[focal_site] = 1;

    /* FIXME Storing the older sites is probably a quite inefficient. */

    /* Work rightwards from the focal site */
    num_older_sites = 0;
    for (l = focal_site + 1; l < (int64_t) self->num_sites; l++) {
        if (self->sites[l].frequency > focal_site_frequency) {
            older_sites[num_older_sites] = l;
            num_older_sites++;
        }
    }
    ancestor_builder_get_consistent_samples(self, focal_site,
            sample_set, &sample_set_size);
    assert(sample_set_size == focal_site_frequency);
    ret = ancestor_builder_compute_older_sites(self,
            focal_site, ancestor,
            older_sites, num_older_sites,
            sample_set, sample_set_size, &last_site);
    if (ret != 0) {
        goto out;
    }
    for (l = focal_site + 1; l < last_site; l++) {
        if (self->sites[l].frequency <= focal_site_frequency) {
            ancestor[l] = 0;
        }
    }
    *ret_end = last_site + 1;

    /* Work leftwards from the focal site */
    num_older_sites = 0;
    for (l = focal_site - 1; l >= 0; l--) {
        if (self->sites[l].frequency > focal_site_frequency) {
            older_sites[num_older_sites] = l;
            num_older_sites++;
        }
    }
    ancestor_builder_get_consistent_samples(self, focal_site,
            sample_set, &sample_set_size);
    assert(sample_set_size == focal_site_frequency);
    ret = ancestor_builder_compute_older_sites(self,
            focal_site, ancestor,
            older_sites, num_older_sites,
            sample_set, sample_set_size, &last_site);
    if (ret != 0) {
        goto out;
    }
    for (l = last_site + 1; l < focal_site; l++) {
        if (self->sites[l].frequency <= focal_site_frequency) {
            ancestor[l] = 0;
        }
    }
    *ret_start = last_site;


        /* assert len(focal_sites) == 1 */
        /* focal_site = focal_sites[0] */
        /* focal_frequency = self.sites[focal_site].frequency */
        /* a[:] = UNKNOWN_ALLELE */
        /* a[focal_site] = 1 */

        /* # Go rightwards from the focal site. */
        /* older_sites = [ */
        /*     l for l in range(focal_site + 1, self.num_sites) */
        /*     if self.sites[l].frequency > focal_frequency] */
        /* last_site = self.compute_older_sites(focal_site, older_sites, a) */
        /* # Fill in the ancestral states at younger sites. */
        /* for l in range(focal_site + 1, last_site): */
        /*     if self.sites[l].frequency <= focal_frequency: */
        /*         a[l] = 0 */
        /* end = last_site + 1 */

        /* # Go leftwards from the focal site. */
        /* older_sites = [ */
        /*     l for l in range(focal_site - 1, -1, -1) */
        /*     if self.sites[l].frequency > focal_frequency] */
        /* last_site = self.compute_older_sites(focal_site, older_sites, a) */
        /* # Fill in the ancestral states at younger sites. */
        /* for l in range(last_site + 1, focal_site): */
        /*     if self.sites[l].frequency <= focal_frequency: */
        /*         a[l] = 0 */
        /* start = last_site */

        /* return start, end */
out:
    tsi_safe_free(older_sites);
    tsi_safe_free(sample_set);
    return ret;
}


int WARN_UNUSED
ancestor_builder_add_site(ancestor_builder_t *self, site_id_t l, size_t frequency,
        allele_t *genotypes)
{
    int ret = 0;
    site_t *site;
    avl_node_t *avl_node;
    site_list_t *list_node;
    pattern_map_t search, *map_elem;
    avl_tree_t *pattern_map = &self->frequency_map[frequency];

    assert(frequency <= self->num_samples);
    assert(l < (site_id_t) self->num_sites);
    site = &self->sites[l];
    site->frequency = frequency;
    if (frequency > 1) {
        search.genotypes = genotypes;
        search.num_samples = self->num_samples;
        avl_node = avl_search(pattern_map, &search);
        if (avl_node == NULL) {
            avl_node = block_allocator_get(&self->allocator, sizeof(avl_node_t));
            map_elem = block_allocator_get(&self->allocator, sizeof(pattern_map_t));
            site->genotypes = block_allocator_get(&self->allocator,
                    self->num_samples * sizeof(allele_t));
            if (avl_node == NULL || map_elem == NULL || site->genotypes == NULL) {
                ret = TSI_ERR_NO_MEMORY;
                goto out;
            }
            memcpy(site->genotypes, genotypes, self->num_samples * sizeof(allele_t));
            avl_init_node(avl_node, map_elem);
            map_elem->genotypes = site->genotypes;
            map_elem->num_samples = self->num_samples;
            map_elem->sites = NULL;
            map_elem->num_sites = 0;
            avl_node = avl_insert_node(pattern_map, avl_node);
            assert(avl_node != NULL);
            if (site->genotypes == NULL) {
                ret = TSI_ERR_NO_MEMORY;
                goto out;
            }
        } else {
            map_elem = (pattern_map_t *) avl_node->item;
            site->genotypes = map_elem->genotypes;
        }
        map_elem->num_sites++;

        list_node = block_allocator_get(&self->allocator, sizeof(site_list_t));
        if (list_node == NULL) {
            ret = TSI_ERR_NO_MEMORY;
            goto out;
        }
        list_node->site = l;
        list_node->next = map_elem->sites;
        map_elem->sites = list_node;
    }
out:
    return ret;
}

static void
ancestor_builder_check_state(ancestor_builder_t *self)
{
    size_t f, k, count;
    avl_node_t *a;
    pattern_map_t *map_elem;
    site_list_t *s;

    for (f = 0; f < self->num_samples + 1; f++) {
        for (a = self->frequency_map[f].head; a != NULL; a = a->next) {
            map_elem = (pattern_map_t *) a->item;
            count = 0;
            for (k = 0; k < self->num_samples; k++) {
                count += map_elem->genotypes[k] == 1;
            }
            assert(count == f);
            count = 0;
            for (s = map_elem->sites; s != NULL; s = s->next) {
                assert(self->sites[s->site].frequency == f);
                assert(self->sites[s->site].genotypes == map_elem->genotypes);
                count++;
            }
            assert(map_elem->num_sites == count);
        }
    }
}

int
ancestor_builder_print_state(ancestor_builder_t *self, FILE *out)
{
    size_t j, k;
    avl_node_t *a;
    pattern_map_t *map_elem;
    site_list_t *s;

    fprintf(out, "Ancestor builder\n");
    fprintf(out, "num_samples = %d\n", (int) self->num_samples);
    fprintf(out, "num_sites = %d\n", (int) self->num_sites);
    fprintf(out, "num_ancestors = %d\n", (int) self->num_ancestors);

    fprintf(out, "Sites:\n");
    for (j = 0; j < self->num_sites; j++) {
        fprintf(out, "%d\t%d\t%p\n", (int) j, (int) self->sites[j].frequency,
                self->sites[j].genotypes);
    }
    fprintf(out, "Frequency map:\n");
    for (j = 0; j < self->num_samples + 1; j++) {
        printf("Frequency = %d: %d ancestors\n", (int) j,
                avl_count(&self->frequency_map[j]));
        for (a = self->frequency_map[j].head; a != NULL; a = a->next) {
            map_elem = (pattern_map_t *) a->item;
            printf("\t");
            for (k = 0; k < self->num_samples; k++) {
                printf("%d", map_elem->genotypes[k]);
            }
            printf("\t");
            for (s = map_elem->sites; s != NULL; s = s->next) {
                printf("%d ", s->site);
            }
            printf("\n");
        }
    }
    fprintf(out, "Descriptors:\n");
    for (j = 0; j < self->num_ancestors; j++) {
        fprintf(out, "%d\t%d: ",  (int) self->descriptors[j].frequency,
                (int) self->descriptors[j].num_focal_sites);
        for (k = 0; k < self->descriptors[j].num_focal_sites; k++) {
            fprintf(out, "%d, ", self->descriptors[j].focal_sites[k]);
        }
        fprintf(out, "\n");
    }
    block_allocator_print_state(&self->allocator, out);
    ancestor_builder_check_state(self);
    return 0;
}

#if 0
FIXME - remove this if we go with one-focal-site-per-ancestor

/* Returns true if we should break the an ancestor that spans from focal
 * site a to focal site b */
static bool
ancestor_builder_break_ancestor(ancestor_builder_t *self, site_id_t a,
        site_id_t b, node_id_t *restrict samples, size_t num_samples)
{
    bool ret = false;
    site_id_t j, k;
    size_t ones;

    for (j = a + 1; j < b && !ret; j++) {
        if (self->sites[j].frequency > self->sites[a].frequency) {
            ones = 0;
            for (k = 0; k < (site_id_t) num_samples; k++) {
                ones += self->sites[j].genotypes[samples[k]];
            }
            if (ones != num_samples && ones != 0) {
                ret = true;
            }
        }
    }
    return ret;
}
#endif

int
ancestor_builder_finalise(ancestor_builder_t *self)
{
    int ret = 0;
    size_t j, k, num_consistent_samples;
    avl_node_t *a;
    pattern_map_t *map_elem;
    site_list_t *s;
    ancestor_descriptor_t *descriptor;
    site_id_t *focal_sites = NULL;
    site_id_t *p;
    site_id_t *consistent_samples = malloc(self->num_samples * sizeof(node_id_t));

    /* TODO we're doing a lot of unnecessary work here joining up sites with identical
     * patterns. Refactor this if we finalise on a single focal site per ancestor */

    if (consistent_samples == NULL) {
        ret = TSI_ERR_NO_MEMORY;
        goto out;
    }
    num_consistent_samples = 0;  /* Keep the compiler happy */
    self->num_ancestors = 0;
    for (j = self->num_samples; j > 1; j--) {
        for (a = self->frequency_map[j].head; a != NULL; a = a->next) {
            descriptor = self->descriptors + self->num_ancestors;
            self->num_ancestors++;
            descriptor->frequency = j;
            map_elem = (pattern_map_t *) a->item;
            focal_sites = block_allocator_get(&self->allocator,
                    map_elem->num_sites * sizeof(site_id_t));
            if (focal_sites == NULL) {
                ret = TSI_ERR_NO_MEMORY;
                goto out;
            }
            descriptor->focal_sites = focal_sites;
            descriptor->num_focal_sites = map_elem->num_sites;
            k = map_elem->num_sites - 1;
            for (s = map_elem->sites; s != NULL; s = s->next) {
                focal_sites[k] = s->site;
                k--;
            }
            /* Now check to see if we need to split this ancestor up
             * further */
            if (map_elem->num_sites > 1) {
                ancestor_builder_get_consistent_samples(self, focal_sites[0],
                        consistent_samples, &num_consistent_samples);
                assert(num_consistent_samples == descriptor->frequency);
            }
            for (k = 0; k < map_elem->num_sites - 1; k++) {
                /* if (ancestor_builder_break_ancestor( */
                /*         self, focal_sites[k], focal_sites[k + 1], */
                /*         consistent_samples, num_consistent_samples)) { */
                if (true) {
                    p = focal_sites + k + 1;
                    descriptor->num_focal_sites = p - descriptor->focal_sites;
                    descriptor = self->descriptors + self->num_ancestors;
                    self->num_ancestors++;
                    descriptor->frequency = j;
                    descriptor->num_focal_sites = map_elem->num_sites - k - 1;
                    descriptor->focal_sites = p;
                }
            }
        }
    }
out:
    tsi_safe_free(consistent_samples);
    return ret;
}
