/*****************************************************************************
 * image.c : wrapper for image reading/writing facilities
 *****************************************************************************
 * Copyright (C) 2004 VideoLAN
 * $Id$
 *
 * Author: Gildas Bazin <gbazin@videolan.org>
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

/**
 * \file
 * This file contains the functions to handle the image_handler_t type
 */

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/decoder.h>
#include <vlc_filter.h>
#include <vlc_image.h>

static picture_t *ImageRead( image_handler_t *, block_t *,
                             video_format_t *, video_format_t * );
static picture_t *ImageReadUrl( image_handler_t *, const char *,
                                video_format_t *, video_format_t * );
static block_t *ImageWrite( image_handler_t *, picture_t *,
                            video_format_t *, video_format_t * );
static int ImageWriteUrl( image_handler_t *, picture_t *,
                          video_format_t *, video_format_t *, const char * );

static decoder_t *CreateDecoder( vlc_object_t *, video_format_t * );
static void DeleteDecoder( decoder_t * );
static filter_t *CreateFilter( vlc_object_t *, es_format_t *,
                               video_format_t * );
static void DeleteFilter( filter_t * );

/**
 * Create an image_handler_t instance
 *
 */
image_handler_t *__image_HandlerCreate( vlc_object_t *p_this )
{
    image_handler_t *p_image = malloc( sizeof(image_handler_t) );

    memset( p_image, 0, sizeof(image_handler_t) );
    p_image->p_parent = p_this;

    p_image->pf_read = ImageRead;
    p_image->pf_read_url = ImageReadUrl;
    p_image->pf_write = ImageWrite;
    p_image->pf_write_url = ImageWriteUrl;

    return p_image;
}

/**
 * Delete the image_handler_t instance
 *
 */
void image_HandlerDelete( image_handler_t *p_image )
{
    if( !p_image ) return;

    if( p_image->p_dec ) DeleteDecoder( p_image->p_dec );
    if( p_image->p_filter ) DeleteFilter( p_image->p_filter );

    free( p_image );
}

/**
 * Read an image
 *
 */

static picture_t *ImageRead( image_handler_t *p_image, block_t *p_block,
                             video_format_t *p_fmt_in,
                             video_format_t *p_fmt_out )
{
    picture_t *p_pic;

    /* Check if we can reuse the current decoder */
    if( p_image->p_dec &&
        p_image->p_dec->fmt_in.i_codec != p_fmt_in->i_chroma )
    {
        DeleteDecoder( p_image->p_dec );
        p_image->p_dec = 0;
    }

    /* Start a decoder */
    if( !p_image->p_dec )
    {
        p_image->p_dec = CreateDecoder( p_image->p_parent, p_fmt_in );
        if( !p_image->p_dec ) return NULL;
    }

    p_block->i_pts = p_block->i_dts = mdate();
    p_pic = p_image->p_dec->pf_decode_video( p_image->p_dec, &p_block );
    p_image->p_dec->pf_decode_video( p_image->p_dec, &p_block );

    if( !p_pic )
    {
        msg_Dbg( p_image->p_parent, "no image decoded" );
        return 0;
    }

    if( !p_fmt_out->i_chroma )
        p_fmt_out->i_chroma = p_image->p_dec->fmt_out.video.i_chroma;
    if( !p_fmt_out->i_width )
        p_fmt_out->i_width = p_image->p_dec->fmt_out.video.i_width;
    if( !p_fmt_out->i_height )
        p_fmt_out->i_height = p_image->p_dec->fmt_out.video.i_height;

    /* Check if we need chroma conversion or resizing */
    if( p_image->p_dec->fmt_out.video.i_chroma != p_fmt_out->i_chroma ||
        p_image->p_dec->fmt_out.video.i_width != p_fmt_out->i_width ||
        p_image->p_dec->fmt_out.video.i_height != p_fmt_out->i_height )
    {
        if( p_image->p_filter )
        if( p_image->p_filter->fmt_in.video.i_chroma !=
            p_image->p_dec->fmt_out.video.i_chroma ||
            p_image->p_filter->fmt_in.video.i_width !=
            p_image->p_dec->fmt_out.video.i_width ||
            p_image->p_filter->fmt_in.video.i_height !=
            p_image->p_dec->fmt_out.video.i_height ||
            p_image->p_filter->fmt_out.video.i_chroma != p_fmt_out->i_chroma ||
            p_image->p_filter->fmt_out.video.i_width != p_fmt_out->i_width ||
            p_image->p_filter->fmt_out.video.i_height != p_fmt_out->i_height )
        {
            /* We need to restart a new filter */
            DeleteFilter( p_image->p_filter );
            p_image->p_filter = 0;
        }

        /* Start a filter */
        if( !p_image->p_filter )
        {
            p_image->p_filter =
                CreateFilter( p_image->p_parent, &p_image->p_dec->fmt_out,
                              p_fmt_out );

            if( !p_image->p_filter )
            {
                p_pic->pf_release( p_pic );
                return NULL;
            }
        }

        p_pic = p_image->p_filter->pf_video_filter( p_image->p_filter, p_pic );
        *p_fmt_out = p_image->p_filter->fmt_out.video;
    }
    else *p_fmt_out = p_image->p_dec->fmt_out.video;

    return p_pic;
}

static picture_t *ImageReadUrl( image_handler_t *p_image, const char *psz_url,
                                video_format_t *p_fmt_in,
                                video_format_t *p_fmt_out )
{
    block_t *p_block;
    picture_t *p_pic;
    FILE *file;
    int i_size;

    file = fopen( psz_url, "rb" );
    if( !file )
    {
        msg_Dbg( p_image->p_parent, "could not open file %s for reading",
                 psz_url );
        return NULL;
    }

    fseek( file, 0, SEEK_END );
    i_size = ftell( file );
    fseek( file, 0, SEEK_SET );

    p_block = block_New( p_image->p_parent, i_size );
    fread( p_block->p_buffer, sizeof(char), i_size, file );
    fclose( file );

    p_pic = ImageRead( p_image, p_block, p_fmt_in, p_fmt_out );

    return p_pic;
}

/**
 * Write an image
 *
 */

static block_t *ImageWrite( image_handler_t *p_image, picture_t *p_pic,
                            video_format_t *p_fmt_in,
                            video_format_t *p_fmt_out )
{
    return NULL;
}

static int ImageWriteUrl( image_handler_t *p_image, picture_t *p_pic,
                          video_format_t *p_fmt_in, video_format_t *p_fmt_out,
                          const char *psz_url )
{
    return VLC_EGENERIC;
}

/**
 * Misc functions
 *
 */
static void video_release_buffer( picture_t *p_pic )
{
    if( p_pic && p_pic->p_data_orig ) free( p_pic->p_data_orig );
    if( p_pic && p_pic->p_sys ) free( p_pic->p_sys );
    if( p_pic ) free( p_pic );
}

static picture_t *video_new_buffer( decoder_t *p_dec )
{
    picture_t *p_pic = malloc( sizeof(picture_t) );

    p_dec->fmt_out.video.i_chroma = p_dec->fmt_out.i_codec;
    vout_AllocatePicture( VLC_OBJECT(p_dec), p_pic,
                          p_dec->fmt_out.video.i_chroma,
                          p_dec->fmt_out.video.i_width,
                          p_dec->fmt_out.video.i_height,
                          p_dec->fmt_out.video.i_aspect );

    if( !p_pic->i_planes )
    {
        free( p_pic );
        return 0;
    }

    p_pic->pf_release = video_release_buffer;
    p_pic->i_status = RESERVED_PICTURE;
    p_pic->p_sys = NULL;

    return p_pic;
}

static void video_del_buffer( decoder_t *p_dec, picture_t *p_pic )
{
    if( p_pic && p_pic->p_data_orig ) free( p_pic->p_data_orig );
    if( p_pic && p_pic->p_sys ) free( p_pic->p_sys );
    if( p_pic ) free( p_pic );
}

static void video_link_picture( decoder_t *p_dec, picture_t *p_pic )
{
}

static void video_unlink_picture( decoder_t *p_dec, picture_t *p_pic )
{
}

static decoder_t *CreateDecoder( vlc_object_t *p_this, video_format_t *fmt )
{
    decoder_t *p_dec;
    static es_format_t null_es_format = {0};

    p_dec = vlc_object_create( p_this, VLC_OBJECT_DECODER );
    if( p_dec == NULL )
    {
        msg_Err( p_this, "out of memory" );
        return NULL;
    }

    p_dec->p_module = NULL;
    es_format_Copy( &p_dec->fmt_in, &null_es_format );
    es_format_Copy( &p_dec->fmt_out, &null_es_format );
    p_dec->fmt_in.video = *fmt;
    p_dec->fmt_in.i_cat = VIDEO_ES;
    p_dec->fmt_in.i_codec = fmt->i_chroma;

    p_dec->pf_vout_buffer_new = video_new_buffer;
    p_dec->pf_vout_buffer_del = video_del_buffer;
    p_dec->pf_picture_link    = video_link_picture;
    p_dec->pf_picture_unlink  = video_unlink_picture;

    vlc_object_attach( p_dec, p_this );

    /* Find a suitable decoder module */
    p_dec->p_module = module_Need( p_dec, "decoder", "$codec", 0 );
    if( !p_dec->p_module )
    {
        msg_Err( p_dec, "no suitable decoder module for fourcc `%4.4s'.\n"
                 "VLC probably does not support this image format.",
                 (char*)&p_dec->fmt_in.i_codec );

        DeleteDecoder( p_dec );
        return NULL;
    }

    return p_dec;
}

static void DeleteDecoder( decoder_t * p_dec )
{
    vlc_object_detach( p_dec );

    if( p_dec->p_module ) module_Unneed( p_dec, p_dec->p_module );

    es_format_Clean( &p_dec->fmt_in );
    es_format_Clean( &p_dec->fmt_out );

    vlc_object_destroy( p_dec );
}

static filter_t *CreateFilter( vlc_object_t *p_this, es_format_t *p_fmt_in,
                               video_format_t *p_fmt_out )
{
    filter_t *p_filter;

    p_filter = vlc_object_create( p_this, VLC_OBJECT_FILTER );
    vlc_object_attach( p_filter, p_this );

    p_filter->pf_vout_buffer_new =
        (picture_t *(*)(filter_t *))video_new_buffer;
    p_filter->pf_vout_buffer_del =
        (void (*)(filter_t *, picture_t *))video_del_buffer;

    p_filter->fmt_in = *p_fmt_in;
    p_filter->fmt_out = *p_fmt_in;
    p_filter->fmt_out.i_codec = p_fmt_out->i_chroma;
    p_filter->fmt_out.video = *p_fmt_out;
    p_filter->p_module = module_Need( p_filter, "video filter2", 0, 0 );

    if( !p_filter->p_module )
    {
        msg_Dbg( p_filter, "no video filter found" );
        DeleteFilter( p_filter );
        return NULL;
    }

    return p_filter;
}

static void DeleteFilter( filter_t * p_filter )
{
    vlc_object_detach( p_filter );

    if( p_filter->p_module ) module_Unneed( p_filter, p_filter->p_module );

    es_format_Clean( &p_filter->fmt_in );
    es_format_Clean( &p_filter->fmt_out );

    vlc_object_destroy( p_filter );
}
