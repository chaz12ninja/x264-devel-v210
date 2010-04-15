/*****************************************************************************
 * lookahead.c: Lookahead slicetype decisions for x264
 *****************************************************************************
 * Lookahead.c and associated modifications:
 *     Copyright (C) 2008 Avail Media
 *
 * Authors: Michael Kazmier <mkazmier@availmedia.com>
 *          Alex Giladi <agiladi@availmedia.com>
 *          Steven Walters <kemuri9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/* LOOKAHEAD (threaded and non-threaded mode)
 *
 * Lookahead types:
 *     [1] Slice type / scene cut;
 *
 * In non-threaded mode, we run the existing slicetype decision code as it was.
 * In threaded mode, we run in a separate thread, that lives between the calls
 * to x264_encoder_open() and x264_encoder_close(), and performs lookahead for
 * the number of frames specified in rc_lookahead.  Recommended setting is
 * # of bframes + # of threads.
 */
#include "common/common.h"
#include "common/cpu.h"
#include "analyse.h"

static void x264_lookahead_shift( x264_synch_frame_list_t *dst, x264_synch_frame_list_t *src, int count )
{
    int i = count;
    while( i-- )
    {
        assert( dst->i_size < dst->i_max_size );
        assert( src->i_size );
        dst->list[ dst->i_size++ ] = x264_frame_shift( src->list );
        src->i_size--;
    }
    if( count )
    {
        x264_pthread_cond_broadcast( &dst->cv_fill );
        x264_pthread_cond_broadcast( &src->cv_empty );
    }
}

static void x264_lookahead_update_last_nonb( x264_t *h, x264_frame_t *new_nonb )
{
    if( h->lookahead->last_nonb )
        x264_frame_push_unused( h, h->lookahead->last_nonb );
    h->lookahead->last_nonb = new_nonb;
    new_nonb->i_reference_count++;
}

#ifdef HAVE_PTHREAD
static void x264_lookahead_slicetype_decide( x264_t *h )
{
    x264_stack_align( x264_slicetype_decide, h );

    x264_lookahead_update_last_nonb( h, h->lookahead->next.list[0] );

    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    while( h->lookahead->ofbuf.i_size == h->lookahead->ofbuf.i_max_size )
        x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_empty, &h->lookahead->ofbuf.mutex );

    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next, h->lookahead->next.list[0]->i_bframes + 1 );
    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );

    /* For MB-tree and VBV lookahead, we have to perform propagation analysis on I-frames too. */
    if( h->lookahead->b_analyse_keyframe && IS_X264_TYPE_I( h->lookahead->last_nonb->i_type ) )
        x264_stack_align( x264_slicetype_analyse, h, 1 );

    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
}

static void x264_lookahead_thread( x264_t *h )
{
    int shift;
#ifdef HAVE_MMX
    if( h->param.cpu&X264_CPU_SSE_MISALIGN )
        x264_cpu_mask_misalign_sse();
#endif
    while( !h->lookahead->b_exit_thread )
    {
        x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
        x264_pthread_mutex_lock( &h->lookahead->next.mutex );
        shift = X264_MIN( h->lookahead->next.i_max_size - h->lookahead->next.i_size, h->lookahead->ifbuf.i_size );
        x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, shift );
        x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
        if( h->lookahead->next.i_size <= h->lookahead->i_slicetype_length )
        {
            while( !h->lookahead->ifbuf.i_size && !h->lookahead->b_exit_thread )
                x264_pthread_cond_wait( &h->lookahead->ifbuf.cv_fill, &h->lookahead->ifbuf.mutex );
            x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
        }
        else
        {
            x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
            x264_lookahead_slicetype_decide( h );
        }
    }   /* end of input frames */
    x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    x264_lookahead_shift( &h->lookahead->next, &h->lookahead->ifbuf, h->lookahead->ifbuf.i_size );
    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
    while( h->lookahead->next.i_size )
        x264_lookahead_slicetype_decide( h );
    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    h->lookahead->b_thread_active = 0;
    x264_pthread_cond_broadcast( &h->lookahead->ofbuf.cv_fill );
    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
}
#endif

int x264_lookahead_init( x264_t *h, int i_slicetype_length )
{
    x264_lookahead_t *look;
    CHECKED_MALLOCZERO( look, sizeof(x264_lookahead_t) );
    for( int i = 0; i < h->param.i_threads; i++ )
        h->thread[i]->lookahead = look;

    look->i_last_keyframe = - h->param.i_keyint_max;
    look->b_analyse_keyframe = (h->param.rc.b_mb_tree || (h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead))
                               && !h->param.rc.b_stat_read;
    look->i_slicetype_length = i_slicetype_length;

    /* init frame lists */
    if( x264_synch_frame_list_init( &look->ifbuf, h->param.i_sync_lookahead+3 ) ||
        x264_synch_frame_list_init( &look->next, h->frames.i_delay+3 ) ||
        x264_synch_frame_list_init( &look->ofbuf, h->frames.i_delay+3 ) )
        goto fail;

    if( !h->param.i_sync_lookahead )
        return 0;

    x264_t *look_h = h->thread[h->param.i_threads];
    *look_h = *h;
    if( x264_macroblock_cache_allocate( look_h ) )
        goto fail;

    if( x264_macroblock_thread_allocate( look_h, 1 ) < 0 )
        goto fail;

    if( x264_pthread_create( &look_h->thread_handle, NULL, (void *)x264_lookahead_thread, look_h ) )
        goto fail;
    look->b_thread_active = 1;

    return 0;
fail:
    x264_free( look );
    return -1;
}

void x264_lookahead_delete( x264_t *h )
{
    if( h->param.i_sync_lookahead )
    {
        x264_pthread_mutex_lock( &h->lookahead->ifbuf.mutex );
        h->lookahead->b_exit_thread = 1;
        x264_pthread_cond_broadcast( &h->lookahead->ifbuf.cv_fill );
        x264_pthread_mutex_unlock( &h->lookahead->ifbuf.mutex );
        x264_pthread_join( h->thread[h->param.i_threads]->thread_handle, NULL );
        x264_macroblock_cache_free( h->thread[h->param.i_threads] );
        x264_macroblock_thread_free( h->thread[h->param.i_threads], 1 );
        x264_free( h->thread[h->param.i_threads] );
    }
    x264_synch_frame_list_delete( &h->lookahead->ifbuf );
    x264_synch_frame_list_delete( &h->lookahead->next );
    if( h->lookahead->last_nonb )
        x264_frame_push_unused( h, h->lookahead->last_nonb );
    x264_synch_frame_list_delete( &h->lookahead->ofbuf );
    x264_free( h->lookahead );
}

void x264_lookahead_put_frame( x264_t *h, x264_frame_t *frame )
{
    if( h->param.i_sync_lookahead )
        x264_synch_frame_list_push( &h->lookahead->ifbuf, frame );
    else
        x264_synch_frame_list_push( &h->lookahead->next, frame );
}

int x264_lookahead_is_empty( x264_t *h )
{
    x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
    x264_pthread_mutex_lock( &h->lookahead->next.mutex );
    int b_empty = !h->lookahead->next.i_size && !h->lookahead->ofbuf.i_size;
    x264_pthread_mutex_unlock( &h->lookahead->next.mutex );
    x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
    return b_empty;
}

static void x264_lookahead_encoder_shift( x264_t *h )
{
    if( !h->lookahead->ofbuf.i_size )
        return;
    int i_frames = h->lookahead->ofbuf.list[0]->i_bframes + 1;
    while( i_frames-- )
    {
        x264_frame_push( h->frames.current, x264_frame_shift( h->lookahead->ofbuf.list ) );
        h->lookahead->ofbuf.i_size--;
    }
    x264_pthread_cond_broadcast( &h->lookahead->ofbuf.cv_empty );
}

void x264_lookahead_get_frames( x264_t *h )
{
    if( h->param.i_sync_lookahead )
    {   /* We have a lookahead thread, so get frames from there */
        x264_pthread_mutex_lock( &h->lookahead->ofbuf.mutex );
        while( !h->lookahead->ofbuf.i_size && h->lookahead->b_thread_active )
            x264_pthread_cond_wait( &h->lookahead->ofbuf.cv_fill, &h->lookahead->ofbuf.mutex );
        x264_lookahead_encoder_shift( h );
        x264_pthread_mutex_unlock( &h->lookahead->ofbuf.mutex );
    }
    else
    {   /* We are not running a lookahead thread, so perform all the slicetype decide on the fly */

        if( h->frames.current[0] || !h->lookahead->next.i_size )
            return;

        x264_stack_align( x264_slicetype_decide, h );
        x264_lookahead_update_last_nonb( h, h->lookahead->next.list[0] );
        x264_lookahead_shift( &h->lookahead->ofbuf, &h->lookahead->next, h->lookahead->next.list[0]->i_bframes + 1 );

        /* For MB-tree and VBV lookahead, we have to perform propagation analysis on I-frames too. */
        if( h->lookahead->b_analyse_keyframe && IS_X264_TYPE_I( h->lookahead->last_nonb->i_type ) )
            x264_stack_align( x264_slicetype_analyse, h, 1 );

        x264_lookahead_encoder_shift( h );
    }
}
