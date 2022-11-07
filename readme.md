UPVC: Common work Dominique Lavenier (INRIA) and UPMEM.
=======================================================

Installation
------------

```
cmake -B build -D NB_DPU=128
cd build
make
```

Run on integration dataset
--------------------------

```
make run
make check
```

Run on other dataset
--------------------

Data-set must consist of 3 files:
  - ``<dataset_prefix>.fasta`` the reference genomee (or part of genomee)
  - ``<dataset_prefix>_PE1.fastq`` the PE1 of the input to compare to the reference
  - ``<dataset_prefix>_PE2.fastq`` the PE2 of the input to compare to the reference
  - ``<reference_vcf> the reference vcf output to check the quality of the computation

Run once to create the MRAM for the reference genomee to compare to:

```
./<path_to_build>/host/upvc -i <dataset_prefix> -n <number_of_virtual_dpus_during_execution> -g index
```

Then run:

```
./<path_to_build>/host/upvc -i <dataset_prefix> -g map  [-n <number_of_physical_dpus_available>]
```

If the number of physical dpus available is not specified, the program will try to alloc every dpus available at runtime.

Result are in ``<dataset_prefix>_upvc.vcf``

To check the quality of the results use:

```
python3 <path_to_build>/../tests/compareVCF.py <reference_vcf> <dataset_prefix>_upvc.vcf
```
