#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

#define TP_INT 0
#define TP_DOUBLE 1


typedef union
{
    double d;
    int64_t i;
    uint64_t u;
} cast_t;

typedef struct
{
    uint64_t ts;
    cast_t val;
} point_t;

typedef struct
{
    uint8_t tp;
    uint16_t len;
    size_t size;
    point_t * data;
} points_t;


points_t * points_new(size_t size, uint8_t tp)
{
    points_t * points = malloc(sizeof(points_t));
    points->len = 0;
    points->tp = tp;
    points->data = malloc(sizeof(point_t) * size);
    return points;
}

points_t * points_copy(points_t * points)
{
    points_t * p = malloc(sizeof(points_t));

    p->len =  points->len;
    p->tp =  points->tp;
    p->data = malloc(sizeof(point_t) * points->len);
    memcpy(p->data, points->data, sizeof(point_t) * points->len);
    return p;
}

void points_destroy(points_t * points)
{
    free(points->data);
    free(points);
}

void points_add_point(
        points_t *__restrict points,
        uint64_t * ts,
        cast_t * val)
{
    size_t i;
    point_t * point;

    for (   i = points->len;
            i-- > 0 && (points->data + i)->ts > *ts;
            *(points->data + i + 1) = *(points->data + i));

    points->len++;
    point = points->data + i + 1;
    point->ts = *ts;
    point->val = *val;
}


const int raw_values_threshold = 7;

unsigned char * zip_int(points_t * points, uint16_t * csz, size_t * size)
{
    const uint64_t store_raw_mask = UINT64_C(0xff80000000000000);
    uint64_t * diff, * prev;
    int64_t * a, * b;
    size_t store_raw_diff = 0;
    uint64_t tdiff = 0;
    uint64_t vdiff = 0;
    unsigned char * bits, *pt;
    uint64_t mask, ts, val;
    size_t i;
    point_t * point;
    int vcount = 0;
    int tcount = 0;
    int shift, bcount;

    i = points->len - 1;
    point = points->data + i;
    for (a = &point->val.i; i--;)
    {
        diff = &point->ts;
        point--;
        *diff -= point->ts;

        b = a;
        a = &point->val.i;

        tdiff |= *diff;

        if (store_raw_diff)
        {
            continue;
        }

        diff = (uint64_t *) b;

        if (*a > *b)
        {
            *diff = *a - *b;
            if (*diff & store_raw_mask)
            {
                store_raw_diff = points->len - i - 1;
                *b = *a - *diff;
                vdiff |= store_raw_mask;
                continue;
            }
            *diff <<= 1;
            *diff |= 1;
        }
        else
        {
            *diff = *b - *a;
            if (*diff & store_raw_mask)
            {
                store_raw_diff = points->len - i - 1;
                *b = *diff + *a;
                vdiff |= store_raw_mask;
                continue;
            }
            *diff <<= 1;
        }

        vdiff |= *diff;
    }

    for (i = 1, mask = 0xff; i <= 8; mask <<= 8, i++)
    {
        if (tdiff & mask)
        {
            tcount = i;
        }
        if (vdiff & mask)
        {
            vcount = i;
        }
    }

    *csz = (uint8_t) store_raw_diff ? 0xff : vcount;
    *csz <<= 8;
    *csz |= (uint8_t) tcount;

    printf("tcount = %d\n", tcount);
    printf("vcount = %d\n", vcount);
    printf("store_raw_diff = %zu\n", store_raw_diff);

    bcount = tcount + vcount;
    *size = 16 + bcount * (points->len - 1);
    bits = (unsigned char *) malloc(*size);
    pt = bits;

    memcpy(pt, &point->ts, sizeof(uint64_t));
    pt += sizeof(uint64_t);
    memcpy(pt, &point->val.u, sizeof(uint64_t));
    pt += sizeof(uint64_t);

    tcount *= 8;
    vcount *= 8;

    for (i = points->len; --i;)
    {
        point++;
        ts = point->ts;
        val = point->val.u;

        for (shift = tcount; (shift -= 8) >= 0; ++pt)
        {
            *pt = ts >> shift;
        }

        if (store_raw_diff)
        {
            if (i < store_raw_diff)
            {
                if (val & 1)
                {
                    val >>= 1;
                    point->val.i = (point-1)->val.i - val;
                }
                else
                {
                    val >>= 1;
                    point->val.i = val + (point-1)->val.i;
                }
                val = point->val.u;
            }

            memcpy(pt, &val, sizeof(uint64_t));
            pt += sizeof(uint64_t);
            continue;
        }

        for (shift = vcount; (shift -= 8) >= 0; ++pt)
        {
            *pt = val >> shift;
        }

    }

    return bits;
}

unsigned char * zip_double(points_t * points, uint16_t * csz, size_t * size)
{
    uint64_t * diff;
    uint64_t tdiff = 0;
    uint64_t mask = points->data->val.u;
    uint64_t vdiff = 0;
    uint8_t vstore = 0;
    uint64_t ts, val;
    size_t i, j;
    point_t * point;
    int tcount = 0;
    int vcount = 0;
    int shift, bcount;
    // int vshift[raw_values_threshold];
    unsigned char * bits, *pt;
    int * pshift;

    i = points->len - 1;
    point = points->data + i;

    while (i--)
    {
        vdiff |= mask ^ point->val.u;
        diff = &point->ts;
        point--;
        *diff -= point->ts;
        tdiff |= *diff;
    }

    for (i = 0, mask = 0xff; i < 8; mask <<= 8, i++)
    {
        if (vdiff & mask)
        {
            vstore |= 1 << i;
            vcount++;
        }
        if (tdiff & mask)
        {
            tcount = i + 1;
        }
    }

    *csz = vstore;
    *csz <<= 8;
    *csz |= (uint8_t) tcount;

    bcount = tcount + vcount;
    *size = 16 + bcount * (points->len - 1);
    bits = (unsigned char *) malloc(*size);
    pt = bits;

    memcpy(pt, &point->ts, sizeof(uint64_t));
    pt += sizeof(uint64_t);
    memcpy(pt, &point->val.u, sizeof(uint64_t));
    pt += sizeof(uint64_t);

    for (i = points->len, tcount *= 8; --i;)
    {
        point++;
        ts = point->ts;
        val = point->val.u;
        for (shift = tcount; (shift -= 8) >= 0; ++pt)
        {
            *pt = ts >> shift;
        }
        if (vcount <= raw_values_threshold)
        {
            for (shift = 0; vstore; vstore >>= 1, shift += 8)
            {
                if (vstore & 1)
                {
                    *pt = val >> shift;
                }
            }
        }
        else
        {
            /* store raw values */
            memcpy(pt, &val, sizeof(uint64_t));
            pt += sizeof(uint64_t);
        }
    }

    return bits;
}

points_t * unzip_int(unsigned char * bits, uint16_t len, uint16_t csz)
{
    points_t * points;
    point_t * point;
    uint8_t vcount, tcount;
    uint64_t ts, tmp;
    int64_t val;
    size_t i, j;
    unsigned char * pt = bits;

    vcount = csz >> 8;
    tcount = csz;

    points = points_new(len, TP_INT);
    point = points->data;

    memcpy(&point->ts, pt, sizeof(uint64_t));
    pt += sizeof(uint64_t);
    memcpy(&point->val.u, pt, sizeof(uint64_t));
    pt += sizeof(uint64_t);

    ts = point->ts;
    val = point->val.i;

    for (i = len; --i;)
    {
        point++;
        for (tmp = 0, j = 0; j < tcount; ++j, ++pt)
        {
            tmp <<= 8;
            tmp |= *pt;
        }
        ts += tmp;
        point->ts = ts;

        if (vcount != 0xff)
        {
            for (tmp = 0, j = 0; j < vcount; ++j, ++pt)
            {
                tmp <<= 8;
                tmp |= *pt;
            }
            val += (tmp & 1) ? -(tmp >> 1) : (tmp >> 1);
            point->val.i = val;
        }
        else
        {
            memcpy(&point->val.u, pt, sizeof(uint64_t));
            pt += sizeof(uint64_t);
        }
    }

    return points;
}

points_t * unzip_double(unsigned char * bits, uint16_t len, uint16_t csz)
{
    points_t * points;
    int vshift[raw_values_threshold];
    int * pshift;
    uint8_t vstore, tcount;
    size_t i, c, j;
    unsigned char * pt = bits;
    uint64_t ts, tmp, mask, val;
    point_t * point;

    vstore = csz >> 8;
    tcount = csz;

    for (   i = 0, c = 0, mask = 0, tmp = 0xff;
            vstore;
            vstore >>= 1, ++i, tmp <<= 8)
    {
        if (vstore & 1)
        {
            vshift[c++] = i * 8;
            mask |= tmp;
        }
    }

    points = points_new(len, TP_DOUBLE);
    point = points->data;

    memcpy(&point->ts, pt, sizeof(uint64_t));
    pt += sizeof(uint64_t);
    memcpy(&point->val.u, pt, sizeof(uint64_t));
    pt += sizeof(uint64_t);

    ts = point->ts;
    val = point->val.u & ~mask;

    for (i = len; --i;)
    {
        point++;
        for (tmp = 0, j = tcount; j--; ++pt)
        {
            tmp <<= 8;
            tmp |= *pt;
        }
        ts += tmp;
        point->ts = ts;

        if (c <= raw_values_threshold)
        {
            for (tmp = 0, pshift = vshift, j = c; j--; ++pshift, ++pt)
            {
                tmp |= ((uint64_t) *pt) << *pshift;
            }
            tmp |= val;
            point->val.u = tmp;
        }
        else
        {
            memcpy(&point->val.u, pt, sizeof(uint64_t));
            pt += sizeof(uint64_t);
        }
    }

    return points;
}

void test_int()
{
    size_t sz = 10;
    points_t * points = points_new(sz, TP_INT);
    for (int i = 0; i < sz; i++)
    {
        int r = rand() % 60;
        uint64_t ts = 1511797596 + i*300 + r;
        cast_t cf;
        cf.i = 1 - i + (r - 30) * r;
        // if (r > 30)
        // {
        //     cf.i = -922337203685477507;
        // }
        points_add_point(points, &ts, &cf);
    }

    uint16_t csz;
    size_t size;
    unsigned char * bits;
    points_t * upoints;

    bits = zip_int(points_copy(points), &csz, &size);
    upoints = unzip_int(bits, points->len, csz);

    printf("size: %lu\n", size);
    free(bits);

    for (size_t i = 0; i < points->len; i++)
    {
        // printf("%lu - %lu\n", points->data[i].ts, upoints->data[i].ts);
        // printf("%lu - %lu\n", points->data[i].val.u, upoints->data[i].val.u);
        assert(points->data[i].ts == upoints->data[i].ts);
        assert(points->data[i].val.u == upoints->data[i].val.u);
    }

    points_destroy(points);
    points_destroy(upoints);

    printf("Finished int test\n");
}

void test_double()
{
    size_t sz = 10;
    points_t * points = points_new(sz, TP_DOUBLE);
    for (int i = 0; i < sz; i++)
    {
        int r = rand() % 60;
        uint64_t ts = 1511797596 + i*300 + r;
        cast_t cf;
        cf.d = 1.0 - (0.01 * i);
        // if (r > 30)
        // {
        //     cf.d = -922337203685477507;
        // }

        points_add_point(points, &ts, &cf);
    }

    uint16_t csz;
    size_t size;
    unsigned char * bits;
    points_t * upoints;

    bits = zip_double(points_copy(points), &csz, &size);
    upoints = unzip_double(bits, points->len, csz);

    printf("size: %lu\n", size);
    free(bits);

    for (size_t i = 0; i < points->len; i++)
    {
        // printf("%lu - %lu\n", points->data[i].ts, upoints->data[i].ts);
        // printf("%lu - %lu\n", points->data[i].val.u, upoints->data[i].val.u);
        assert(points->data[i].ts == upoints->data[i].ts);
        assert(points->data[i].val.u == upoints->data[i].val.u);
    }

    points_destroy(points);
    points_destroy(upoints);


    printf("Finished double test\n");
}

int main()
{
    srand(time(NULL));
    size_t sz = 10;
    test_int();
    test_double();
}
