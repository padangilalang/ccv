#include "ccv.h"
#ifdef HAVE_GSL
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#endif
#ifndef _WIN32
#include <sys/time.h>
#endif
#ifdef USE_OPENMP
#include <omp.h>
#endif
#ifdef USE_OPENCL
#include <CL/cl.h>
#endif

#define __ccv_width_padding(x) (((x) + 3) & -4)

static unsigned int __ccv_bbf_time_measure()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

static inline int __ccv_run_bbf_feature(ccv_bbf_feature_t* feature, int* step, unsigned char** u8)
{
#define pf_at(i) (*(u8[feature->pz[i]] + feature->px[i] + feature->py[i] * step[feature->pz[i]]))
#define nf_at(i) (*(u8[feature->nz[i]] + feature->nx[i] + feature->ny[i] * step[feature->nz[i]]))
	unsigned char pmin = pf_at(0), nmax = nf_at(0);
	/* check if every point in P > every point in N, and take a shortcut */
	if (pmin <= nmax)
		return 0;
	int i;
	for (i = 1; i < feature->size; i++)
	{
		if (feature->pz[i] >= 0)
		{
			int p = pf_at(i);
			if (p < pmin)
			{
				if (p <= nmax)
					return 0;
				pmin = p;
			}
		}
		if (feature->nz[i] >= 0)
		{
			int n = nf_at(i);
			if (n > nmax)
			{
				if (pmin <= n)
					return 0;
				nmax = n;
			}
		}
	}
#undef pf_at
#undef nf_at
	return 1;
}

#define less_than(a, b, aux) ((a) < (b))
CCV_IMPLEMENT_QSORT(__ccv_sort_32f, float, less_than)
#undef less_than

static void __ccv_bbf_eval_data(ccv_bbf_stage_classifier_t* classifier, unsigned char** posdata, int posnum, unsigned char** negdata, int negnum, ccv_size_t size, float* peval, float* neval)
{
	int i, j;
	int steps[] = { __ccv_width_padding(size.width),
					__ccv_width_padding(size.width >> 1),
					__ccv_width_padding(size.width >> 2) };
	int isizs0 = steps[0] * size.height;
	int isizs01 = isizs0 + steps[1] * (size.height >> 1);
	for (i = 0; i < posnum; i++)
	{
		unsigned char* u8[] = { posdata[i], posdata[i] + isizs0, posdata[i] + isizs01 };
		float sum = 0;
		float* alpha = classifier->alpha;
		ccv_bbf_feature_t* feature = classifier->feature;
		for (j = 0; j < classifier->count; ++j, alpha += 2, ++feature)
			sum += alpha[__ccv_run_bbf_feature(feature, steps, u8)];
		peval[i] = sum;
	}
	for (i = 0; i < negnum; i++)
	{
		unsigned char* u8[] = { negdata[i], negdata[i] + isizs0, negdata[i] + isizs01 };
		float sum = 0;
		float* alpha = classifier->alpha;
		ccv_bbf_feature_t* feature = classifier->feature;
		for (j = 0; j < classifier->count; ++j, alpha += 2, ++feature)
			sum += alpha[__ccv_run_bbf_feature(feature, steps, u8)];
		neval[i] = sum;
	}
}

static int __ccv_prune_positive_data(ccv_bbf_classifier_cascade_t* cascade, unsigned char** posdata, int posnum, ccv_size_t size)
{
	float* peval = (float*)malloc(posnum * sizeof(float));
	int i, j, k, rpos = posnum;
	for (i = 0; i < cascade->count; i++)
	{
		__ccv_bbf_eval_data(cascade->stage_classifier + i, posdata, rpos, 0, 0, size, peval, 0);
		k = 0;
		for (j = 0; j < rpos; j++)
			if (peval[j] >= cascade->stage_classifier[i].threshold)
			{
				posdata[k] = posdata[j];
				++k;
			} else {
				free(posdata[j]);
			}
		rpos = k;
	}
	free(peval);
	return rpos;
}

static int __ccv_prepare_background_data(ccv_bbf_classifier_cascade_t* cascade, char** bgfiles, int bgnum, unsigned char** negdata, int negnum)
{
	int t, i, j, k;
	int negperbg = negnum / bgnum + 1;
	int negtotal = 0;
	int steps[] = { __ccv_width_padding(cascade->size.width),
					__ccv_width_padding(cascade->size.width >> 1),
					__ccv_width_padding(cascade->size.width >> 2) };
	int isizs0 = steps[0] * cascade->size.height;
	int isizs1 = steps[1] * (cascade->size.height >> 1);
	int isizs2 = steps[2] * (cascade->size.height >> 2);
	printf("preparing negative data ...  0%%");
	int* idcheck = (int*)malloc(negnum * sizeof(int));

	gsl_rng_env_setup();

	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	gsl_rng_set(rng, (unsigned long int)idcheck);

	ccv_size_t imgsz = cascade->size;
	int rneg = negtotal;
	for (t = 0; negtotal < negnum; t++)
	{
		for (i = 0; i < bgnum; i++)
		{
			negperbg = (t < 2) ? (negnum - negtotal) / (bgnum - i) + 1 : negnum - negtotal;
			ccv_dense_matrix_t* image = 0;
			ccv_unserialize(bgfiles[i], &image, CCV_SERIAL_GRAY | CCV_SERIAL_ANY_FILE);
			assert((image->type & CCV_C1) && (image->type & CCV_8U));
			if (image == 0)
			{
				printf("\n%s file corrupted\n", bgfiles[i]);
				continue;
			}
			if (t % 2 != 0)
				ccv_flip(image, 0, 0, CCV_FLIP_X);
			ccv_array_t* detected = ccv_bbf_detect_objects(image, &cascade, 1, 0, 0, cascade->size);
			for (j = 0; j < ccv_min(detected->rnum, negperbg); j++)
			{
				int r = gsl_rng_uniform_int(rng, detected->rnum);
				int flag = 1;
				ccv_rect_t* rect = (ccv_rect_t*)ccv_array_get(detected, r);
				while (flag) {
					flag = 0;
					for (k = 0; k < j; k++)
						if (r == idcheck[k])
						{
							flag = 1;
							r = gsl_rng_uniform_int(rng, detected->rnum);
							break;
						}
					rect = (ccv_rect_t*)ccv_array_get(detected, r);
					if ((rect->x < 0) || (rect->y < 0) || (rect->width + rect->x >= image->cols) || (rect->height + rect->y >= image->rows))
					{
						flag = 1;
						r = gsl_rng_uniform_int(rng, detected->rnum);
					}
				}
				idcheck[j] = r;
				ccv_dense_matrix_t* temp = 0;
				ccv_dense_matrix_t* imgs0 = 0;
				ccv_dense_matrix_t* imgs1 = 0;
				ccv_dense_matrix_t* imgs2 = 0;
				ccv_slice(image, (ccv_matrix_t**)&temp, 0, rect->y, rect->x, rect->height, rect->width);
				ccv_resample(temp, &imgs0, 0, imgsz.height, imgsz.width, CCV_INTER_AREA);
				ccv_matrix_free(temp);
				ccv_sample_down(imgs0, &imgs1, 0);
				ccv_sample_down(imgs1, &imgs2, 0);

				negdata[negtotal] = (unsigned char*)malloc(isizs0 + isizs1 + isizs2);
				unsigned char* u8s0 = negdata[negtotal];
				unsigned char* u8s1 = negdata[negtotal] + isizs0;
				unsigned char* u8s2 = negdata[negtotal] + isizs0 + isizs1;
				unsigned char* u8[] = { u8s0, u8s1, u8s2 };
				memcpy(u8s0, imgs0->data.ptr, imgs0->rows * imgs0->step);
				ccv_matrix_free(imgs0);
				memcpy(u8s1, imgs1->data.ptr, imgs1->rows * imgs1->step);
				ccv_matrix_free(imgs1);
				memcpy(u8s2, imgs2->data.ptr, imgs2->rows * imgs2->step);
				ccv_matrix_free(imgs2);

				flag = 1;
				ccv_bbf_stage_classifier_t* classifier = cascade->stage_classifier;
				for (k = 0; k < cascade->count; ++k, ++classifier)
				{
					float sum = 0;
					float* alpha = classifier->alpha;
					ccv_bbf_feature_t* feature = classifier->feature;
					for (k = 0; k < classifier->count; ++k, alpha += 2, ++feature)
						sum += alpha[__ccv_run_bbf_feature(feature, steps, u8)];
					if (sum < classifier->threshold)
					{
						flag = 0;
						break;
					}
				}
				if (!flag)
					free(negdata[negtotal]);
				else {
					++negtotal;
					if (negtotal >= negnum)
						break;
				}
			}
			ccv_array_free(detected);
			ccv_matrix_free(image);
			ccv_garbage_collect();
			printf("\rpreparing negative data ... %2d%%", 100 * negtotal / negnum);
			fflush(0);
			if (negtotal >= negnum)
				break;
		}
		if (rneg == negtotal)
			break;
		rneg = negtotal;
	}
	gsl_rng_free(rng);
	free(idcheck);
	ccv_garbage_collect();
	printf("\n");
	return negtotal;
}

static void __ccv_prepare_positive_data(ccv_dense_matrix_t** posimg, unsigned char** posdata, ccv_size_t size, int posnum)
{
	printf("preparing positive data ...  0%%");
	int i;
	for (i = 0; i < posnum; i++)
	{
		ccv_dense_matrix_t* imgs0 = posimg[i];
		ccv_dense_matrix_t* imgs1 = 0;
		ccv_dense_matrix_t* imgs2 = 0;
		assert((imgs0->type & CCV_C1) && (imgs0->type & CCV_8U) && imgs0->rows == size.height && imgs0->cols == size.width);
		ccv_sample_down(imgs0, &imgs1, 0);
		ccv_sample_down(imgs1, &imgs2, 0);
		int isizs0 = imgs0->rows * imgs0->step;
		int isizs1 = imgs1->rows * imgs1->step;
		int isizs2 = imgs2->rows * imgs2->step;

		posdata[i] = (unsigned char*)malloc(isizs0 + isizs1 + isizs2);
		memcpy(posdata[i], imgs0->data.ptr, isizs0);
		memcpy(posdata[i] + isizs0, imgs1->data.ptr, isizs1);
		memcpy(posdata[i] + isizs0 + isizs1, imgs2->data.ptr, isizs2);

		printf("\rpreparing positive data ... %2d%%", 100 * (i + 1) / posnum);
		fflush(0);

		ccv_matrix_free(imgs1);
		ccv_matrix_free(imgs2);
	}
	ccv_garbage_collect();
	printf("\n");
}

typedef struct {
	double fitness;
	int pk, nk;
	int age;
	double error;
	ccv_bbf_feature_t feature;
} ccv_bbf_gene_t;

static inline void __ccv_bbf_genetic_fitness(ccv_bbf_gene_t* gene)
{
	gene->fitness = (1 - gene->error) * exp(-0.01 * gene->age) * exp((gene->pk + gene->nk) * log(1.015));
}

static inline int __ccv_bbf_exist_gene_feature(ccv_bbf_gene_t* gene, int x, int y, int z)
{
	int i;
	for (i = 0; i < gene->pk; i++)
		if (z == gene->feature.pz[i] && x == gene->feature.px[i] && y == gene->feature.py[i])
			return 1;
	for (i = 0; i < gene->nk; i++)
		if (z == gene->feature.nz[i] && x == gene->feature.nx[i] && y == gene->feature.ny[i])
			return 1;
	return 0;
}

static inline int __ccv_bbf_identical_feature(ccv_bbf_gene_t* ga, ccv_bbf_gene_t* gb)
{
	if (ga->pk != gb->pk || ga->nk != gb->nk)
		return 0;
	int i, j, k;
	for (i = 0; i < ga->pk; i++)
	{
		k = 0;
		for (j = 0; j < gb->pk; j++)
			if (ga->feature.pz[i] == gb->feature.pz[j] && ga->feature.px[i] == gb->feature.px[j] && ga->feature.py[i] == gb->feature.py[j])
			{
				k = 1;
				break;
			}
		if (!k)
			return 0;
	}
	for (i = 0; i < ga->nk; i++)
	{
		k = 0;
		for (j = 0; j < gb->nk; j++)
			if (ga->feature.nz[i] == gb->feature.nz[j] && ga->feature.nx[i] == gb->feature.nx[j] && ga->feature.ny[i] == gb->feature.ny[j])
			{
				k = 1;
				break;
			}
		if (!k)
			return 0;
	}
	return 1;
}

static inline void __ccv_bbf_randomize_gene(gsl_rng* rng, ccv_bbf_gene_t* gene, int* rows, int* cols)
{
	int i;
	do {
		gene->pk = gsl_rng_uniform_int(rng, CCV_BBF_POINT_MAX - 1) + 1;
		gene->nk = gsl_rng_uniform_int(rng, CCV_BBF_POINT_MAX - 1) + 1;
	} while (gene->pk + gene->nk < CCV_BBF_POINT_MIN); /* a hard restriction of at least 3 points have to be examed */
	gene->feature.size = ccv_max(gene->pk, gene->nk);
	gene->age = 0;
	for (i = 0; i < CCV_BBF_POINT_MAX; i++)
	{
		gene->feature.pz[i] = -1;
		gene->feature.nz[i] = -1;
	}
	int x, y, z;
	for (i = 0; i < gene->pk; i++)
	{
		do {
			z = gsl_rng_uniform_int(rng, 3);
			x = gsl_rng_uniform_int(rng, cols[z]);
			y = gsl_rng_uniform_int(rng, rows[z]);
		} while (__ccv_bbf_exist_gene_feature(gene, x, y, z));
		gene->feature.pz[i] = z;
		gene->feature.px[i] = x;
		gene->feature.py[i] = y;
	}
	for (i = 0; i < gene->nk; i++)
	{
		do {
			z = gsl_rng_uniform_int(rng, 3);
			x = gsl_rng_uniform_int(rng, cols[z]);
			y = gsl_rng_uniform_int(rng, rows[z]);
		} while ( __ccv_bbf_exist_gene_feature(gene, x, y, z));
		gene->feature.nz[i] = z;
		gene->feature.nx[i] = x;
		gene->feature.ny[i] = y;
	}
}

static inline double __ccv_bbf_error_rate(ccv_bbf_feature_t* feature, unsigned char** posdata, int posnum, unsigned char** negdata, int negnum, ccv_size_t size, double* pw, double* nw)
{
	int i;
	int steps[] = { __ccv_width_padding(size.width),
					__ccv_width_padding(size.width >> 1),
					__ccv_width_padding(size.width >> 2) };
	int isizs0 = steps[0] * size.height;
	int isizs01 = isizs0 + steps[1] * (size.height >> 1);
	double error = 0;
	for (i = 0; i < posnum; i++)
	{
		unsigned char* u8[] = { posdata[i], posdata[i] + isizs0, posdata[i] + isizs01 };
		if (!__ccv_run_bbf_feature(feature, steps, u8))
			error += pw[i];
	}
	for (i = 0; i < negnum; i++)
	{
		unsigned char* u8[] = { negdata[i], negdata[i] + isizs0, negdata[i] + isizs01 };
		if ( __ccv_run_bbf_feature(feature, steps, u8))
			error += nw[i];
	}
	return error;
}

static ccv_bbf_gene_t __ccv_bbf_best_gene(ccv_bbf_gene_t* gene, int pnum, unsigned char** posdata, int posnum, unsigned char** negdata, int negnum, ccv_size_t size, double* pw, double* nw)
{
	int i;
	unsigned int timer = __ccv_bbf_time_measure();
#ifdef USE_OPENMP
#pragma omp parallel for private(i) schedule(dynamic)
#endif
	for (i = 0; i < pnum; i++)
		gene[i].error = __ccv_bbf_error_rate(&gene[i].feature, posdata, posnum, negdata, negnum, size, pw, nw);
	timer = __ccv_bbf_time_measure() - timer;
	int min_id = 0;
	double min_err = gene[0].error;
	for (i = 1; i < pnum; i++)
		if (gene[i].error < min_err)
		{
			min_id = i;
			min_err = gene[i].error;
		}
	printf("local best bbf feature with error %f\n|-size: %d\n|-positive point: ", min_err, gene[min_id].feature.size);
	for (i = 0; i < gene[min_id].feature.size; i++)
		printf("(%d %d %d), ", gene[min_id].feature.px[i], gene[min_id].feature.py[i], gene[min_id].feature.pz[i]);
	printf("\n|-negative point: ");
	for (i = 0; i < gene[min_id].feature.size; i++)
		printf("(%d %d %d), ", gene[min_id].feature.nx[i], gene[min_id].feature.ny[i], gene[min_id].feature.nz[i]);
	printf("\nthe computation takes %d ms\n", timer / 1000);
	return gene[min_id];
}

static ccv_bbf_feature_t __ccv_bbf_convex_optimize(unsigned char** posdata, int posnum, unsigned char** negdata, int negnum, int ftnum, ccv_size_t size, double* pw, double* nw)
{
	/* seed (random method) */
	gsl_rng_env_setup();
	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	union { unsigned long int li; double db; } dbli;
	dbli.db = pw[0] + nw[0];
	gsl_rng_set(rng, dbli.li);
	int i, j, k, q, g;
	int rows[] = { size.height, size.height >> 1, size.height >> 2 };
	int cols[] = { size.width, size.width >> 1, size.width >> 2 };
	int pnum = rows[0] * cols[0] + rows[1] * cols[1] + rows[2] * cols[2];
	ccv_bbf_gene_t* gene = (ccv_bbf_gene_t*)malloc((pnum * (CCV_BBF_POINT_MAX + 1) * 2 + CCV_BBF_POINT_MAX * 2 + 1) * sizeof(ccv_bbf_gene_t));
	ccv_bbf_gene_t best_gene;
	/* bootstrapping the best feature, start from two pixels, one for positive, one for negative
	 * the bootstrapping process go like this: first, it will assign a random pixel as positive
	 * and enumerate every possible pixel as negative, and pick the best one. Then, enumerate every
	 * possible pixel as positive, and pick the best one, until it converges */
	memset(&best_gene, 0, sizeof(ccv_bbf_gene_t));
	for (i = 0; i < CCV_BBF_POINT_MAX; i++)
		best_gene.feature.pz[i] = best_gene.feature.nz[i] = -1;
	best_gene.pk = 1;
	best_gene.nk = 0;
	best_gene.feature.size = 1;
	best_gene.feature.pz[0] = gsl_rng_uniform_int(rng, 3);
	best_gene.feature.px[0] = gsl_rng_uniform_int(rng, cols[best_gene.feature.pz[0]]);
	best_gene.feature.py[0] = gsl_rng_uniform_int(rng, rows[best_gene.feature.pz[0]]);
	int t;
	for (t = 0; ; ++t)
	{
		if (t % 2 == 0)
		{
			g = 0;
			for (i = 0; i < 3; i++)
				for (j = 0; j < cols[i]; j++)
					for (k = 0; k < rows[i]; k++)
						if (i != best_gene.feature.pz[0] && j != best_gene.feature.px[0] && k != best_gene.feature.py[0])
						{
							gene[g] = best_gene;
							gene[g].pk = gene[g].nk = 1;
							gene[g].feature.nz[0] = i;
							gene[g].feature.nx[0] = j;
							gene[g].feature.ny[0] = k;
							g++;
						}
		} else {
			g = 0;
			for (i = 0; i < 3; i++)
				for (j = 0; j < cols[i]; j++)
					for (k = 0; k < rows[i]; k++)
						if (i != best_gene.feature.nz[0] && j != best_gene.feature.nx[0] && k != best_gene.feature.ny[0])
						{
							gene[g] = best_gene;
							gene[g].pk = gene[g].nk = 1;
							gene[g].feature.pz[0] = i;
							gene[g].feature.px[0] = j;
							gene[g].feature.py[0] = k;
							g++;
						}
		}
		printf("bootstrapping round : %d\n", t);
		ccv_bbf_gene_t local_gene = __ccv_bbf_best_gene(gene, g, posdata, posnum, negdata, negnum, size, pw, nw);
		if (__ccv_bbf_identical_feature(&local_gene, &best_gene))
			break;
		best_gene = local_gene;
	}
	/* after bootstrapping, the float search technique will do the following permutations:
	 * a). add a new point to positive or negative
	 * b). remove a point from positive or negative
	 * c). move an existing point in positive or negative to another position
	 * the three rules applied exhaustively, no heuristic used. */
	for (t = 0; ; ++t)
	{
		int g = 0;
		for (i = 0; i < 3; i++)
			for (j = 0; j < cols[i]; j++)
				for (k = 0; k < rows[i]; k++)
					if (!__ccv_bbf_exist_gene_feature(&best_gene, j, k, i))
					{
						if (best_gene.pk < CCV_BBF_POINT_MAX - 1)
						{
							gene[g] = best_gene;
							gene[g].feature.pz[gene[g].pk] = i;
							gene[g].feature.px[gene[g].pk] = j;
							gene[g].feature.py[gene[g].pk] = k;
							gene[g].pk++;
							gene[g].feature.size = ccv_max(gene[g].pk, gene[g].nk);
							g++;
						}
						if (best_gene.nk < CCV_BBF_POINT_MAX - 1)
						{
							gene[g] = best_gene;
							gene[g].feature.nz[gene[g].nk] = i;
							gene[g].feature.nx[gene[g].nk] = j;
							gene[g].feature.ny[gene[g].nk] = k;
							gene[g].nk++;
							gene[g].feature.size = ccv_max(gene[g].pk, gene[g].nk);
							g++;
						}
						for (q = 0; q < best_gene.pk; q++)
						{
							gene[g] = best_gene;
							gene[g].feature.pz[q] = i;
							gene[g].feature.px[q] = j;
							gene[g].feature.py[q] = k;
							g++;
						}
						for (q = 0; q < best_gene.nk; q++)
						{
							gene[g] = best_gene;
							gene[g].feature.nz[q] = i;
							gene[g].feature.nx[q] = j;
							gene[g].feature.ny[q] = k;
							g++;
						}
					}
		if (best_gene.pk > 1)
			for (q = 0; q < best_gene.pk; q++)
			{
				gene[g] = best_gene;
				for (i = q; i < best_gene.pk; i++)
				{
					gene[g].feature.pz[i] = gene[g].feature.pz[i + 1];
					gene[g].feature.px[i] = gene[g].feature.px[i + 1];
					gene[g].feature.py[i] = gene[g].feature.py[i + 1];
				}
				gene[g].feature.pz[gene[g].pk - 1] = -1;
				gene[g].pk--;
				gene[g].feature.size = ccv_max(gene[g].pk, gene[g].nk);
				g++;
			}
		if (best_gene.nk > 1)
			for (q = 0; q < best_gene.nk; q++)
			{
				gene[g] = best_gene;
				for (i = q; i < best_gene.nk; i++)
				{
					gene[g].feature.nz[i] = gene[g].feature.nz[i + 1];
					gene[g].feature.nx[i] = gene[g].feature.nx[i + 1];
					gene[g].feature.ny[i] = gene[g].feature.ny[i + 1];
				}
				gene[g].feature.nz[gene[g].nk - 1] = -1;
				gene[g].nk--;
				gene[g].feature.size = ccv_max(gene[g].pk, gene[g].nk);
				g++;
			}
		gene[g] = best_gene;
		g++;
		printf("float search round : %d\n", t);
		ccv_bbf_gene_t local_gene = __ccv_bbf_best_gene(gene, g, posdata, posnum, negdata, negnum, size, pw, nw);
		if (__ccv_bbf_identical_feature(&local_gene, &best_gene))
			break;
		best_gene = local_gene;
	}
	gsl_rng_free(rng);
	return best_gene.feature;
}

static int __ccv_read_bbf_stage_classifier(const char* file, ccv_bbf_stage_classifier_t* classifier)
{
	FILE* r = fopen(file, "r");
	if (r == 0) return -1;
	int stat = 0;
	stat |= fscanf(r, "%d", &classifier->count);
	union { float fl; int i; } fli;
	stat |= fscanf(r, "%d", &fli.i);
	classifier->threshold = fli.fl;
	classifier->feature = (ccv_bbf_feature_t*)malloc(classifier->count * sizeof(ccv_bbf_feature_t));
	classifier->alpha = (float*)malloc(classifier->count * 2 * sizeof(float));
	int i, j;
	for (i = 0; i < classifier->count; i++)
	{
		stat |= fscanf(r, "%d", &classifier->feature[i].size);
		for (j = 0; j < classifier->feature[i].size; j++)
		{
			stat |= fscanf(r, "%d %d %d", &classifier->feature[i].px[j], &classifier->feature[i].py[j], &classifier->feature[i].pz[j]);
			stat |= fscanf(r, "%d %d %d", &classifier->feature[i].nx[j], &classifier->feature[i].ny[j], &classifier->feature[i].nz[j]);
		}
		union { float fl; int i; } flia, flib;
		stat |= fscanf(r, "%d %d", &flia.i, &flib.i);
		classifier->alpha[i * 2] = flia.fl;
		classifier->alpha[i * 2 + 1] = flib.fl;
	}
	fclose(r);
	return 0;
}

static int __ccv_write_bbf_stage_classifier(const char* file, ccv_bbf_stage_classifier_t* classifier)
{
	FILE* w = fopen(file, "wb");
	if (w == 0) return -1;
	fprintf(w, "%d\n", classifier->count);
	union { float fl; int i; } fli;
	fli.fl = classifier->threshold;
	fprintf(w, "%d\n", fli.i);
	int i, j;
	for (i = 0; i < classifier->count; i++)
	{
		fprintf(w, "%d\n", classifier->feature[i].size);
		for (j = 0; j < classifier->feature[i].size; j++)
		{
			fprintf(w, "%d %d %d\n", classifier->feature[i].px[j], classifier->feature[i].py[j], classifier->feature[i].pz[j]);
			fprintf(w, "%d %d %d\n", classifier->feature[i].nx[j], classifier->feature[i].ny[j], classifier->feature[i].nz[j]);
		}
		union { float fl; int i; } flia, flib;
		flia.fl = classifier->alpha[i * 2];
		flib.fl = classifier->alpha[i * 2 + 1];
		fprintf(w, "%d %d\n", flia.i, flib.i);
	}
	fclose(w);
	return 0;
}

static int __ccv_read_background_data(const char* file, unsigned char** negdata, int* negnum, ccv_size_t size)
{
	int stat = 0;
	FILE* r = fopen(file, "rb");
	if (r == 0) return -1;
	stat |= fread(negnum, sizeof(int), 1, r);
	int i;
	int isizs012 = __ccv_width_padding(size.width) * size.height +
				   __ccv_width_padding(size.width >> 1) * (size.height >> 1) +
				   __ccv_width_padding(size.width >> 2) * (size.height >> 2);
	for (i = 0; i < *negnum; i++)
	{
		negdata[i] = (unsigned char*)malloc(isizs012);
		stat |= fread(negdata[i], 1, isizs012, r);
	}
	fclose(r);
	return 0;
}

static int __ccv_write_background_data(const char* file, unsigned char** negdata, int negnum, ccv_size_t size)
{
	FILE* w = fopen(file, "w");
	if (w == 0) return -1;
	fwrite(&negnum, sizeof(int), 1, w);
	int i;
	int isizs012 = __ccv_width_padding(size.width) * size.height +
				   __ccv_width_padding(size.width >> 1) * (size.height >> 1) +
				   __ccv_width_padding(size.width >> 2) * (size.height >> 2);
	for (i = 0; i < negnum; i++)
		fwrite(negdata[i], 1, isizs012, w);
	fclose(w);
	return 0;
}

static int __ccv_resume_bbf_cascade_training_state(const char* file, int* i, int* k, int* bg, double* pw, double* nw, int posnum, int negnum)
{
	int stat = 0;
	FILE* r = fopen(file, "r");
	if (r == 0) return -1;
	stat |= fscanf(r, "%d %d %d", i, k, bg);
	int j;
	union { double db; int i[2]; } dbi;
	for (j = 0; j < posnum; j++)
	{
		stat |= fscanf(r, "%d %d", &dbi.i[0], &dbi.i[1]);
		pw[j] = dbi.db;
	}
	for (j = 0; j < negnum; j++)
	{
		stat |= fscanf(r, "%d %d", &dbi.i[0], &dbi.i[1]);
		nw[j] = dbi.db;
	}
	fclose(r);
	return 0;
}

static int __ccv_save_bbf_cacade_training_state(const char* file, int i, int k, int bg, double* pw, double* nw, int posnum, int negnum)
{
	FILE* w = fopen(file, "w");
	if (w == 0) return -1;
	fprintf(w, "%d %d %d\n", i, k, bg);
	int j;
	union { double db; int i[2]; } dbi;
	for (j = 0; j < posnum; ++j)
	{
		dbi.db = pw[j];
		fprintf(w, "%d %d ", dbi.i[0], dbi.i[1]);
	}
	fprintf(w, "\n");
	for (j = 0; j < negnum; ++j)
	{
		dbi.db = nw[j];
		fprintf(w, "%d %d ", dbi.i[0], dbi.i[1]);
	}
	fprintf(w, "\n");
	fclose(w);
	return 0;
}

void ccv_bbf_classifier_cascade_new(ccv_dense_matrix_t** posimg, int posnum, char** bgfiles, int bgnum, int negnum, ccv_size_t size, const char* dir, ccv_bbf_param_t params)
{
	int i, j, k;
	/* allocate memory for usage */
	ccv_bbf_classifier_cascade_t* cascade = (ccv_bbf_classifier_cascade_t*)malloc(sizeof(ccv_bbf_classifier_cascade_t));
	cascade->count = 0;
	cascade->size = size;
	cascade->stage_classifier = (ccv_bbf_stage_classifier_t*)malloc(sizeof(ccv_bbf_stage_classifier_t));
	unsigned char** posdata = (unsigned char**)malloc(posnum * sizeof(unsigned char*));
	unsigned char** negdata = (unsigned char**)malloc(negnum * sizeof(unsigned char*));
	double* pw = (double*)malloc(posnum * sizeof(double));
	double* nw = (double*)malloc(negnum * sizeof(double));
	float* peval = (float*)malloc(posnum * sizeof(float));
	float* neval = (float*)malloc(negnum * sizeof(float));
	double inv_balance_k = 1. / params.balance_k;
	/* balance factor k, and weighted with 0.01 */
	params.balance_k *= 0.01;
	inv_balance_k *= 0.01;

	int steps[] = { __ccv_width_padding(cascade->size.width),
					__ccv_width_padding(cascade->size.width >> 1),
					__ccv_width_padding(cascade->size.width >> 2) };
	int isizs0 = steps[0] * cascade->size.height;
	int isizs01 = isizs0 + steps[1] * (cascade->size.height >> 1);
	
	i = 0;
	k = 0;
	int bg = 0;
	int cacheK = 10;
	/* state resume code */
	char buf[1024];
	sprintf(buf, "%s/stat.txt", dir);
	__ccv_resume_bbf_cascade_training_state(buf, &i, &k, &bg, pw, nw, posnum, negnum);
	if (i > 0)
	{
		cascade->count = i;
		free(cascade->stage_classifier);
		cascade->stage_classifier = (ccv_bbf_stage_classifier_t*)malloc(i * sizeof(ccv_bbf_stage_classifier_t));
		for (j = 0; j < i; j++)
		{
			sprintf(buf, "%s/stage-%d.txt", dir, j);
			__ccv_read_bbf_stage_classifier(buf, &cascade->stage_classifier[j]);
		}
	}
	if (k > 0)
		cacheK = k;
	int rpos, rneg;
	if (bg)
	{
		sprintf(buf, "%s/negs.txt", dir);
		__ccv_read_background_data(buf, negdata, &rneg, cascade->size);
	}

	for (; i < params.layer; i++)
	{
		if (!bg)
		{
			rneg = __ccv_prepare_background_data(cascade, bgfiles, bgnum, negdata, negnum);
			/* save state of background data */
			sprintf(buf, "%s/negs.txt", dir);
			__ccv_write_background_data(buf, negdata, rneg, cascade->size);
			bg = 1;
		}
		double totalw;
		/* save state of cascade : level, weight etc. */
		sprintf(buf, "%s/stat.txt", dir);
		__ccv_save_bbf_cacade_training_state(buf, i, k, bg, pw, nw, posnum, negnum);
		ccv_bbf_stage_classifier_t classifier;
		if (k > 0)
		{
			/* resume state of classifier */
			sprintf( buf, "%s/stage-%d.txt", dir, i );
			__ccv_read_bbf_stage_classifier(buf, &classifier);
		} else {
			/* initialize classifier */
			for (j = 0; j < posnum; j++)
				pw[j] = params.balance_k;
			for (j = 0; j < rneg; j++)
				nw[j] = inv_balance_k;
			classifier.count = k;
			classifier.threshold = 0;
			classifier.feature = (ccv_bbf_feature_t*)malloc(cacheK * sizeof(ccv_bbf_feature_t));
			classifier.alpha = (float*)malloc(cacheK * 2 * sizeof(float));
		}
		__ccv_prepare_positive_data(posimg, posdata, cascade->size, posnum);
		rpos = __ccv_prune_positive_data(cascade, posdata, posnum, cascade->size);
		printf("%d postivie data and %d negative data in training\n", rpos, rneg);
		/* reweight to 1.00 */
		totalw = 0;
		for (j = 0; j < rpos; j++)
			totalw += pw[j];
		for (j = 0; j < rneg; j++)
			totalw += nw[j];
		for (j = 0; j < rpos; j++)
			pw[j] = pw[j] / totalw;
		for (j = 0; j < rneg; j++)
			nw[j] = nw[j] / totalw;
		for (; ; k++)
		{
			/* get overall true-positive, false-positive rate and threshold */
			double tp = 0, fp = 0, etp = 0, efp = 0;
			__ccv_bbf_eval_data(&classifier, posdata, rpos, negdata, rneg, cascade->size, peval, neval);
			__ccv_sort_32f(peval, rpos, 0);
			classifier.threshold = peval[(int)((1. - params.pos_crit) * rpos)] - 1e-6;
			for (j = 0; j < rpos; j++)
			{
				if (peval[j] >= 0)
					++tp;
				if (peval[j] >= classifier.threshold)
					++etp;
			}
			tp /= rpos; etp /= rpos;
			for (j = 0; j < rneg; j++)
			{
				if (neval[j] >= 0)
					++fp;
				if (neval[j] >= classifier.threshold)
					++efp;
			}
			fp /= rneg; efp /= rneg;
			printf("stage classifier real TP rate : %f, FP rate : %f\n", tp, fp);
			printf("stage classifier TP rate : %f, FP rate : %f at threshold : %f\n", etp, efp, classifier.threshold);
			if (k > 0)
			{
				/* save classifier state */
				sprintf(buf, "%s/stage-%d.txt", dir, i);
				__ccv_write_bbf_stage_classifier(buf, &classifier);
				sprintf(buf, "%s/stat.txt", dir);
				__ccv_save_bbf_cacade_training_state(buf, i, k, bg, pw, nw, posnum, negnum);
			}
			if (etp > params.pos_crit && efp < params.neg_crit)
				break;
			/* TODO: more post-process is needed in here */

			/* select the best feature in current distribution through genetic algorithm optimization */
			ccv_bbf_feature_t best = __ccv_bbf_convex_optimize(posdata, rpos, negdata, rneg, params.feature_number, cascade->size, pw, nw);
			double err = __ccv_bbf_error_rate(&best, posdata, rpos, negdata, rneg, cascade->size, pw, nw);
			double rw = (1 - err) / err;
			totalw = 0;
			/* reweight */
			for (j = 0; j < rpos; j++)
			{
				unsigned char* u8[] = { posdata[j], posdata[j] + isizs0, posdata[j] + isizs01 };
				if (!__ccv_run_bbf_feature(&best, steps, u8))
					pw[j] *= rw;
				pw[j] *= params.balance_k;
				totalw += pw[j];
			}
			for (j = 0; j < rneg; j++)
			{
				unsigned char* u8[] = { negdata[j], negdata[j] + isizs0, negdata[j] + isizs01 };
				if (__ccv_run_bbf_feature(&best, steps, u8))
					nw[j] *= rw;
				nw[j] *= inv_balance_k;
				totalw += nw[j];
			}
			for (j = 0; j < rpos; j++)
				pw[j] = pw[j] / totalw;
			for (j = 0; j < rneg; j++)
				nw[j] = nw[j] / totalw;
			double c = log(rw);
			printf("coefficient of feature %d: %f\n", k + 1, c);
			classifier.count = k + 1;
			/* resizing classifier */
			if (k >= cacheK)
			{
				ccv_bbf_feature_t* feature = (ccv_bbf_feature_t*)malloc(cacheK * 2 * sizeof(ccv_bbf_feature_t));
				memcpy(feature, classifier.feature, cacheK * sizeof(ccv_bbf_feature_t));
				free(classifier.feature);
				float* alpha = (float*)malloc(cacheK * 4 * sizeof(float));
				memcpy(alpha, classifier.alpha, cacheK * 2 * sizeof(float));
				free(classifier.alpha);
				classifier.feature = feature;
				classifier.alpha = alpha;
				cacheK *= 2;
			}
			/* setup new feature */
			classifier.feature[k] = best;
			classifier.alpha[k * 2] = -c;
			classifier.alpha[k * 2 + 1] = c;
		}
		cascade->count = i + 1;
		ccv_bbf_stage_classifier_t* stage_classifier = (ccv_bbf_stage_classifier_t*)malloc(cascade->count * sizeof(ccv_bbf_stage_classifier_t));
		memcpy(stage_classifier, cascade->stage_classifier, i * sizeof(ccv_bbf_stage_classifier_t));
		free(cascade->stage_classifier);
		stage_classifier[i] = classifier;
		cascade->stage_classifier = stage_classifier;
		k = 0;
		bg = 0;
		for (j = 0; j < rpos; j++)
			free(posdata[j]);
		for (j = 0; j < rneg; j++)
			free(negdata[j]);
	}

	free(neval);
	free(peval);
	free(nw);
	free(pw);
	free(negdata);
	free(posdata);
	free(cascade);
}

static int __ccv_is_equal(const void* _r1, const void* _r2, void* data)
{
	const ccv_bbf_comp_t* r1 = (const ccv_bbf_comp_t*)_r1;
	const ccv_bbf_comp_t* r2 = (const ccv_bbf_comp_t*)_r2;
	int distance = (int)(r1->rect.width * 0.5 + 0.5);

	return r2->rect.x <= r1->rect.x + distance &&
		   r2->rect.x >= r1->rect.x - distance &&
		   r2->rect.y <= r1->rect.y + distance &&
		   r2->rect.y >= r1->rect.y - distance &&
		   r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		   (int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width;
}

static int __ccv_is_equal_same_class(const void* _r1, const void* _r2, void* data)
{
	const ccv_bbf_comp_t* r1 = (const ccv_bbf_comp_t*)_r1;
	const ccv_bbf_comp_t* r2 = (const ccv_bbf_comp_t*)_r2;
	int distance = (int)(r1->rect.width * 0.5 + 0.5);

	return r2->id == r1->id &&
		   r2->rect.x <= r1->rect.x + distance &&
		   r2->rect.x >= r1->rect.x - distance &&
		   r2->rect.y <= r1->rect.y + distance &&
		   r2->rect.y >= r1->rect.y - distance &&
		   r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		   (int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width;
}

ccv_array_t* ccv_bbf_detect_objects(ccv_dense_matrix_t* a, ccv_bbf_classifier_cascade_t** _cascade, int count, int min_neighbors, int flags, ccv_size_t min_size)
{
	int hr = a->rows / min_size.height;
	int wr = a->cols / min_size.width;
	int scale_upto = (int)(log((double)ccv_min(hr, wr)) / log(sqrt(2.)));
	/* generate scale-down HOG images */
	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + 4) * sizeof(ccv_dense_matrix_t*));
	if (min_size.height != _cascade[0]->size.height || min_size.width != _cascade[0]->size.width)
	{
		pyr[0] = 0;
		ccv_resample(a, &pyr[0], 0, a->rows * _cascade[0]->size.height / min_size.height, a->cols * _cascade[0]->size.width / min_size.width, CCV_INTER_AREA);
	} else
		pyr[0] = a;
	double sqrt_2 = sqrt(2.);
	pyr[1] = 0;
	ccv_resample(pyr[0], &pyr[1], 0, (int)(pyr[0]->rows / sqrt_2), (int)(pyr[0]->cols / sqrt_2), CCV_INTER_AREA);
	int i, j, k, t, x, y;
	for (i = 2; i < scale_upto + 4; i += 2)
	{
		pyr[i] = 0;
		ccv_sample_down(pyr[i - 2], &pyr[i], 0);
	}
	for ( i = 3; i < scale_upto + 4; i += 2 )
	{
		pyr[i] = 0;
		ccv_sample_down(pyr[i - 2], &pyr[i], 0);
	}
	ccv_array_t* idx_seq;
	ccv_array_t* seq = ccv_array_new(64, sizeof(ccv_bbf_comp_t));
	ccv_array_t* seq2 = ccv_array_new(64, sizeof(ccv_bbf_comp_t));
	ccv_array_t* result_seq = ccv_array_new(64, sizeof(ccv_bbf_comp_t));
	/* detect in multi scale */
	for (t = 0; t < count; t++)
	{
		ccv_bbf_classifier_cascade_t* cascade = _cascade[t];
		float scale_x = (float) min_size.width / (float) cascade->size.width;
		float scale_y = (float) min_size.height / (float) cascade->size.height;
		ccv_array_clear(seq);
		for (i = 0; i < scale_upto; i++)
		{
			int i_rows = pyr[i + 4]->rows - (cascade->size.height >> 1);
			int steps[] = { pyr[i]->step, pyr[i + 2]->step, pyr[i + 4]->step };
			int i_cols = pyr[i + 4]->cols - (cascade->size.width >> 1);
			int paddings[] = { pyr[i]->step - i_cols * 4,
							   pyr[i + 2]->step - i_cols * 2,
							   pyr[i + 4]->step - i_cols };
			unsigned char* u8[] = { pyr[i]->data.ptr, pyr[i + 2]->data.ptr, pyr[i + 4]->data.ptr };
			for (y = 0; y < i_rows; y++)
			{
				for (x = 0; x < i_cols; x++)
				{
					float sum;
					int flag = 1;
					ccv_bbf_stage_classifier_t* classifier = cascade->stage_classifier;
					for (j = 0; j < cascade->count; ++j, ++classifier)
					{
						sum = 0;
						float* alpha = classifier->alpha;
						ccv_bbf_feature_t* feature = classifier->feature;
						for (k = 0; k < classifier->count; ++k, alpha += 2, ++feature)
							sum += alpha[__ccv_run_bbf_feature(feature, steps, u8)];
						if (sum < classifier->threshold)
						{
							flag = 0;
							break;
						}
					}
					if (flag)
					{
						ccv_bbf_comp_t comp;
						comp.rect = ccv_rect((int)(x * 4 * scale_x), (int)(y * 4 * scale_y), (int)(cascade->size.width * scale_x), (int)(cascade->size.height * scale_y));
						comp.id = t;
						comp.neighbors = 1;
						comp.confidence = sum;
						ccv_array_push(seq, &comp);
					}
					u8[0] += 4;
					u8[1] += 2;
					u8[2] += 1;
				}
				u8[0] += paddings[0];
				u8[1] += paddings[1];
				u8[2] += paddings[2];
			}
			scale_x *= sqrt_2;
			scale_y *= sqrt_2;
		}

		/* the following code from OpenCV's haar feature implementation */
		if(min_neighbors == 0)
		{
			for (i = 0; i < seq->rnum; i++)
			{
				ccv_bbf_comp_t* comp = (ccv_bbf_comp_t*)ccv_array_get(seq, i);
				ccv_array_push(result_seq, comp);
			}
		} else {
			idx_seq = 0;
			ccv_array_clear(seq2);
			// group retrieved rectangles in order to filter out noise
			int ncomp = ccv_array_group(seq, &idx_seq, __ccv_is_equal_same_class, 0);
			ccv_bbf_comp_t* comps = (ccv_bbf_comp_t*)malloc((ncomp + 1) * sizeof(ccv_bbf_comp_t));
			memset(comps, 0, (ncomp + 1) * sizeof(ccv_bbf_comp_t));

			// count number of neighbors
			for(i = 0; i < seq->rnum; i++)
			{
				ccv_bbf_comp_t r1 = *(ccv_bbf_comp_t*)ccv_array_get(seq, i);
				int idx = *(int*)ccv_array_get(idx_seq, i);

				if (comps[idx].neighbors == 0)
					comps[idx].confidence = r1.confidence;

				++comps[idx].neighbors;

				comps[idx].rect.x += r1.rect.x;
				comps[idx].rect.y += r1.rect.y;
				comps[idx].rect.width += r1.rect.width;
				comps[idx].rect.height += r1.rect.height;
				comps[idx].id = r1.id;
				comps[idx].confidence = ccv_max(comps[idx].confidence, r1.confidence);
			}

			// calculate average bounding box
			for(i = 0; i < ncomp; i++)
			{
				int n = comps[i].neighbors;
				if(n >= min_neighbors)
				{
					ccv_bbf_comp_t comp;
					comp.rect.x = (comps[i].rect.x * 2 + n) / (2 * n);
					comp.rect.y = (comps[i].rect.y * 2 + n) / (2 * n);
					comp.rect.width = (comps[i].rect.width * 2 + n) / (2 * n);
					comp.rect.height = (comps[i].rect.height * 2 + n) / (2 * n);
					comp.neighbors = comps[i].neighbors;
					comp.id = comps[i].id;
					comp.confidence = comps[i].confidence;
					ccv_array_push(seq2, &comp);
				}
			}

			// filter out small face rectangles inside large face rectangles
			for(i = 0; i < seq2->rnum; i++)
			{
				ccv_bbf_comp_t r1 = *(ccv_bbf_comp_t*)ccv_array_get(seq2, i);
				int flag = 1;

				for(j = 0; j < seq2->rnum; j++)
				{
					ccv_bbf_comp_t r2 = *(ccv_bbf_comp_t*)ccv_array_get(seq2, j);
					int distance = (int)(r2.rect.width * 0.5 + 0.5);

					if(i != j &&
					   r1.id == r2.id &&
					   r1.rect.x >= r2.rect.x - distance &&
					   r1.rect.y >= r2.rect.y - distance &&
					   r1.rect.x + r1.rect.width <= r2.rect.x + r2.rect.width + distance &&
					   r1.rect.y + r1.rect.height <= r2.rect.y + r2.rect.height + distance &&
					   (r2.neighbors > ccv_max(3, r1.neighbors) || r1.neighbors < 3))
					{
						flag = 0;
						break;
					}
				}

				if(flag)
					ccv_array_push(result_seq, &r1);
			}
			ccv_array_free(idx_seq);
			free(comps);
		}
	}

	ccv_array_free(seq);
	ccv_array_free(seq2);

	ccv_array_t* result_seq2;
	/* the following code from OpenCV's haar feature implementation */
	if (flags & CCV_SGF_NO_NESTED)
	{
		result_seq2 = ccv_array_new(64, sizeof(ccv_bbf_comp_t));
		idx_seq = 0;
		// group retrieved rectangles in order to filter out noise
		int ncomp = ccv_array_group(result_seq, &idx_seq, __ccv_is_equal, 0);
		ccv_bbf_comp_t* comps = (ccv_bbf_comp_t*)malloc((ncomp + 1) * sizeof(ccv_bbf_comp_t));
		memset(comps, 0, (ncomp + 1) * sizeof(ccv_bbf_comp_t));

		// count number of neighbors
		for(i = 0; i < result_seq->rnum; i++)
		{
			ccv_bbf_comp_t r1 = *(ccv_bbf_comp_t*)ccv_array_get(result_seq, i);
			int idx = *(int*)ccv_array_get(idx_seq, i);

			if (comps[idx].neighbors == 0 || comps[idx].confidence < r1.confidence)
			{
				comps[idx].confidence = r1.confidence;
				comps[idx].neighbors = 1;
				comps[idx].rect = r1.rect;
				comps[idx].id = r1.id;
			}
		}

		// calculate average bounding box
		for(i = 0; i < ncomp; i++)
			if(comps[i].neighbors)
				ccv_array_push(result_seq2, &comps[i]);

		ccv_array_free(result_seq);
		free(comps);
	} else {
		result_seq2 = result_seq;
	}

	for (i = 1; i < scale_upto + 4; i++)
		ccv_matrix_free(pyr[i]);
	if (min_size.height != _cascade[0]->size.height || min_size.width != _cascade[0]->size.width)
		ccv_matrix_free(pyr[0]);

	return result_seq2;
}

ccv_bbf_classifier_cascade_t* ccv_load_bbf_classifier_cascade(const char* directory)
{
	ccv_bbf_classifier_cascade_t* cascade = (ccv_bbf_classifier_cascade_t*)malloc(sizeof(ccv_bbf_classifier_cascade_t));
	char buf[1024];
	sprintf(buf, "%s/cascade.txt", directory);
	int s, i;
	FILE* r = fopen(buf, "r");
	if (r == 0) return 0;
	s = fscanf(r, "%d %d %d", &cascade->count, &cascade->size.width, &cascade->size.height);
	cascade->stage_classifier = (ccv_bbf_stage_classifier_t*)malloc(cascade->count * sizeof(ccv_bbf_stage_classifier_t));
	for (i = 0; i < cascade->count; i++)
	{
		sprintf(buf, "%s/stage-%d.txt", directory, i);
		if (__ccv_read_bbf_stage_classifier(buf, &cascade->stage_classifier[i]) < 0)
		{
			cascade->count = i;
			break;
		}
	}
	fclose(r);
	return cascade;
}

ccv_bbf_classifier_cascade_t* ccv_bbf_classifier_cascade_read_binary(char* s)
{
	int i;
	ccv_bbf_classifier_cascade_t* cascade = (ccv_bbf_classifier_cascade_t*)malloc(sizeof(ccv_bbf_classifier_cascade_t));
	memcpy(&cascade->count, s, sizeof(cascade->count)); s += sizeof(cascade->count);
	memcpy(&cascade->size.width, s, sizeof(cascade->size.width)); s += sizeof(cascade->size.width);
	memcpy(&cascade->size.height, s, sizeof(cascade->size.height)); s += sizeof(cascade->size.height);
	ccv_bbf_stage_classifier_t* classifier = cascade->stage_classifier = (ccv_bbf_stage_classifier_t*)malloc(cascade->count * sizeof(ccv_bbf_stage_classifier_t));
	for (i = 0; i < cascade->count; i++, classifier++)
	{
		memcpy(&classifier->count, s, sizeof(classifier->count)); s += sizeof(classifier->count);
		memcpy(&classifier->threshold, s, sizeof(classifier->threshold)); s += sizeof(classifier->threshold);
		classifier->feature = (ccv_bbf_feature_t*)malloc(classifier->count * sizeof(ccv_bbf_feature_t));
		classifier->alpha = (float*)malloc(classifier->count * 2 * sizeof(float));
		memcpy(classifier->feature, s, classifier->count * sizeof(ccv_bbf_feature_t)); s += classifier->count * sizeof(ccv_bbf_feature_t);
		memcpy(classifier->alpha, s, classifier->count * 2 * sizeof(float)); s += classifier->count * 2 * sizeof(float);
	}
	return cascade;

}

int ccv_bbf_classifier_cascade_write_binary(ccv_bbf_classifier_cascade_t* cascade, char* s, int slen)
{
	int i;
	int len = sizeof(cascade->count) + sizeof(cascade->size.width) + sizeof(cascade->size.height);
	ccv_bbf_stage_classifier_t* classifier = cascade->stage_classifier;
	for (i = 0; i < cascade->count; i++, classifier++)
		len += sizeof(classifier->count) + sizeof(classifier->threshold) + classifier->count * sizeof(ccv_bbf_feature_t) + classifier->count * 2 * sizeof(float);
	if (slen >= len)
	{
		memcpy(s, &cascade->count, sizeof(cascade->count)); s += sizeof(cascade->count);
		memcpy(s, &cascade->size.width, sizeof(cascade->size.width)); s += sizeof(cascade->size.width);
		memcpy(s, &cascade->size.height, sizeof(cascade->size.height)); s += sizeof(cascade->size.height);
		classifier = cascade->stage_classifier;
		for (i = 0; i < cascade->count; i++, classifier++)
		{
			memcpy(s, &classifier->count, sizeof(classifier->count)); s += sizeof(classifier->count);
			memcpy(s, &classifier->threshold, sizeof(classifier->threshold)); s += sizeof(classifier->threshold);
			memcpy(s, classifier->feature, classifier->count * sizeof(ccv_bbf_feature_t)); s += classifier->count * sizeof(ccv_bbf_feature_t);
			memcpy(s, classifier->alpha, classifier->count * 2 * sizeof(float)); s += classifier->count * 2 * sizeof(float);
		}
	}
	return len;
}

void ccv_bbf_classifier_cascade_free(ccv_bbf_classifier_cascade_t* cascade)
{
	int i;
	for (i = 0; i < cascade->count; ++i)
	{
		free(cascade->stage_classifier[i].feature);
		free(cascade->stage_classifier[i].alpha);
	}
	free(cascade->stage_classifier);
	free(cascade);
}