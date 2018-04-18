import numpy as np
import random
import os
import h5py
import zarr
import sys
import pandas as pd
import daiquiri
#import bsddb3
import time
import scipy
import pickle
import collections
import itertools
import tqdm
import shutil

import matplotlib as mp
# Force matplotlib to not use any Xwindows backend.
mp.use('Agg')
import matplotlib.pyplot as plt
import seaborn as sns

import tsinfer
import msprime



def plot_breakpoints(ts, map_file, output_file):
    # Read in the recombination map using the read_hapmap method,
    recomb_map = msprime.RecombinationMap.read_hapmap(map_file)

    # Now we get the positions and rates from the recombination
    # map and plot these using 500 bins.
    positions = np.array(recomb_map.get_positions()[1:])
    rates = np.array(recomb_map.get_rates()[1:])
    num_bins = 500
    v, bin_edges, _ = scipy.stats.binned_statistic(
        positions, rates, bins=num_bins)
    x = bin_edges[:-1][np.logical_not(np.isnan(v))]
    y = v[np.logical_not(np.isnan(v))]
    fig, ax1 = plt.subplots(figsize=(16, 6))
    ax1.plot(x, y, color="blue", label="Recombination rate")
    ax1.set_ylabel("Recombination rate")
    ax1.set_xlabel("Chromosome position")

    # Now plot the density of breakpoints along the chromosome
    breakpoints = np.array(list(ts.breakpoints()))
    ax2 = ax1.twinx()
    v, bin_edges = np.histogram(breakpoints, num_bins, density=True)
    ax2.plot(bin_edges[:-1], v, color="green", label="Breakpoint density")
    ax2.set_ylabel("Breakpoint density")
    ax2.set_xlim(1.5e7, 5.3e7)
    plt.legend()
    fig.savefig(output_file)


def make_errors(v, p):
    """
    For each sample an error occurs with probability p. Errors are generated by
    sampling values from the stationary distribution, that is, if we have an
    allele frequency of f, a 1 is emitted with probability f and a
    0 with probability 1 - f. Thus, there is a possibility that an 'error'
    will in fact result in the same value.
    """
    w = np.copy(v)
    if p > 0:
        m = v.shape[0]
        frequency = np.sum(v) / m
        # Randomly choose samples with probability p
        samples = np.where(np.random.random(m) < p)[0]
        # Generate observations from the stationary distribution.
        errors = (np.random.random(samples.shape[0]) < frequency).astype(int)
        w[samples] = errors
    return w


def generate_samples(ts, error_p):
    """
    Returns samples with a bits flipped with a specified probability.

    Rejects any variants that result in a fixed column.
    """
    S = np.zeros((ts.sample_size, ts.num_mutations), dtype=np.int8)
    for variant in ts.variants():
        done = False
        # Reject any columns that have no 1s or no zeros
        while not done:
            S[:, variant.index] = make_errors(variant.genotypes, error_p)
            s = np.sum(S[:, variant.index])
            done = 0 < s < ts.sample_size
    return S.T


def tsinfer_dev(
        n, L, seed, num_threads=1, recombination_rate=1e-8,
        error_rate=0, method="C", log_level="WARNING",
        debug=True, progress=False, path_compression=True):

    np.random.seed(seed)
    random.seed(seed)
    L_megabases = int(L * 10**6)

    # daiquiri.setup(level=log_level)

    ts = msprime.simulate(
            n, Ne=10**4, length=L_megabases,
            recombination_rate=recombination_rate, mutation_rate=1e-8,
            random_seed=seed)
    if debug:
        print("num_sites = ", ts.num_sites)
    assert ts.num_sites > 0

    G = generate_samples(ts, error_rate)
    sample_data = tsinfer.SampleData.initialise(
        num_samples=ts.num_samples, sequence_length=ts.sequence_length)
    for site, genotypes in zip(ts.sites(), G):
        sample_data.add_variant(site.position, ["0", "1"], genotypes)
    sample_data.finalise()

    ancestor_data = tsinfer.AncestorData.initialise(sample_data)
    tsinfer.build_ancestors(sample_data, ancestor_data, method=method)
    ancestor_data.finalise()

    ancestors_ts = tsinfer.match_ancestors(sample_data, ancestor_data, method=method)
    output_ts = tsinfer.match_samples(sample_data, ancestors_ts, method=method)
    print("inferred_num_edges = ", output_ts.num_edges)

    A = ancestor_data.genotypes[:].T
    A[A == 255] = 0
    for v in ancestors_ts.variants():
        assert np.array_equal(v.genotypes, A[:, v.index])

    assert output_ts.num_samples == ts.num_samples
    assert output_ts.num_sites == ts.num_sites
    assert output_ts.sequence_length == ts.sequence_length
    assert np.array_equal(G, output_ts.genotype_matrix())


def build_profile_inputs(n, num_megabases):
    L = num_megabases * 10**6
    ts = msprime.simulate(
        n, length=L, Ne=10**4, recombination_rate=1e-8, mutation_rate=1e-8,
        random_seed=10)
    print("Ran simulation: n = ", n, " num_sites = ", ts.num_sites,
            "num_trees =", ts.num_trees)
    input_file = "tmp__NOBACKUP__/profile-n={}-m={}.input.hdf5".format(
            n, num_megabases)
    ts.dump(input_file)
    filename = "tmp__NOBACKUP__/profile-n={}_m={}.samples".format(n, num_megabases)
    if os.path.exists(filename):
        shutil.rmtree(filename)
    sample_data = tsinfer.SampleData.initialise(
        num_samples=ts.num_samples, sequence_length=ts.sequence_length,
        filename=filename)
    progress_monitor = tqdm.tqdm(total=ts.num_sites)
    for variant in ts.variants():
        sample_data.add_variant(
            variant.site.position, variant.alleles, variant.genotypes)
        progress_monitor.update()
    sample_data.finalise()
    progress_monitor.close()

#     filename = "tmp__NOBACKUP__/profile-n={}_m={}.ancestors".format(n, num_megabases)
#     if os.path.exists(filename):
#         os.unlink(filename)
#     ancestor_data = tsinfer.AncestorData.initialise(sample_data, filename=filename)
#     tsinfer.build_ancestors(sample_data, ancestor_data, progress=True)
#     ancestor_data.finalise()


def build_1kg_sim():
    n = 5008
    chrom = "22"
    infile = "data/hapmap/genetic_map_GRCh37_chr{}.txt".format(chrom)
    recomb_map = msprime.RecombinationMap.read_hapmap(infile)

    # ts = msprime.simulate(
    #     sample_size=n, Ne=10**4, recombination_map=recomb_map,
    #     mutation_rate=5*1e-8)

    # print("simulated chr{} with {} sites".format(chrom, ts.num_sites))

    prefix = "tmp__NOBACKUP__/sim1kg_chr{}".format(chrom)
    outfile = prefix + ".source.ts"
    # ts.dump(outfile)
    ts = msprime.load(outfile)

    V = ts.genotype_matrix()
    print("Built variant matrix: {:.2f} MiB".format(V.nbytes / (1024 * 1024)))
    positions = np.array([site.position for site in ts.sites()])
    recombination_rates = np.zeros_like(positions)
    last_physical_pos = 0
    last_genetic_pos = 0
    for site in ts.sites():
        physical_pos = site.position
        genetic_pos = recomb_map.physical_to_genetic(physical_pos)
        physical_dist = physical_pos - last_physical_pos
        genetic_dist = genetic_pos - last_genetic_pos
        scaled_recomb_rate = 0
        if genetic_dist > 0:
            scaled_recomb_rate = physical_dist / genetic_dist
        recombination_rates[site.index] = scaled_recomb_rate
        last_physical_pos = physical_pos
        last_genetic_pos = genetic_pos

    input_file = prefix + ".tsinf"
    if os.path.exists(input_file):
        os.unlink(input_file)
    input_hdf5 = zarr.DBMStore(input_file, open=bsddb3.btopen)
    root = zarr.group(store=input_hdf5, overwrite=True)
    tsinfer.InputFile.build(
        root, genotypes=V, position=positions,
        recombination_rate=recombination_rates, sequence_length=ts.sequence_length)
    input_hdf5.close()
    print("Wrote", input_file)


if __name__ == "__main__":

    np.set_printoptions(linewidth=20000)
    np.set_printoptions(threshold=20000000)


    # build_profile_inputs(10, 1)
    # build_profile_inputs(1000, 10)
    # build_profile_inputs(1000, 100)
    # build_profile_inputs(10**4, 100)
    # build_profile_inputs(10**5, 100)

    tsinfer_dev(8, 0.1, seed=6, num_threads=0, method="C")

#     for seed in range(1, 10000):
#         print(seed)
#         # tsinfer_dev(40, 2.5, seed=seed, num_threads=1, genotype_quality=1e-3, method="C")

