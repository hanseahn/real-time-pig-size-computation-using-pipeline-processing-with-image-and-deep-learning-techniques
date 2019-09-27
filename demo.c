#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "image.h"
#include "demo.h"
#ifdef WIN32
#include <time.h>
#include "gettimeofday.h"
#else
#include <sys/time.h>
#endif


#ifdef OPENCV

#include "http_stream.h"

static long long int frame_id = 0;
static network net;
static int count = 0;

static char **demo_names;
static image **demo_alphabet[3];
static int demo_classes;

static int nboxes[3] = { 0 };
static detection *dets[3] = { NULL };


static image in_s[3] ;
static image det_s[3];

static cap_cv *cap;
static float nms = .45;    // 0.4F
static float fps = 0;
static float demo_thresh =  0 ;
static int demo_ext_output[3] = { 0 };
static int demo_json_port[3] = { -1 };
static int thresh_GMM[3] = { 0 };
CvPixelBackgroundGMM* pGMM =  0 ;

#define NFRAMES 3

static float* predictions[NFRAMES];
static int demo_index = 0;
static image images[NFRAMES];
static mat_cv* cv_images[NFRAMES];
static float *avg;

mat_cv* in_img[3];
mat_cv* det_img[3];
mat_cv* show_img[3];

mat_cv* Hist_img1[3];
mat_cv* in_img_resize[3];
mat_cv* GMM_img[3];
mat_cv* AND_img[3];
mat_cv* area_img[3];
mat_cv* edge_img[3];
mat_cv* Merge_img[3];

static volatile int flag_exit;
static int letter_box =  0 ;

static int Global_id = 0;
static int Pre_id = 0;
static int Mid_id = 0;
static int Post_id = 0;

void *GMM_work(void *ptr) {
    in_img_resize[Global_id%3] = img_resize(in_img[Global_id % 3]);
    Hist_img1[Global_id % 3] = HistEqual(in_img_resize[Global_id % 3]);
    GMM_img[Global_id % 3] = GMM_update(pGMM, in_img_resize[Global_id % 3]); //GMM+median
    thresh_GMM[Global_id % 3] = Count_Non_Zero(GMM_img[Global_id % 3]);
    return 0;

}

void *fetch_in_thread(void *ptr)
{
    int dont_close_stream = 0;    // set 1 if your IP-camera periodically turns off and turns on video-stream
    if(letter_box)
        in_s[Global_id % 3] = get_image_from_stream_letterbox(cap, net.w, net.h, net.c, &in_img[Global_id % 3], dont_close_stream);
    else
        in_s[Global_id % 3] = get_image_from_stream_resize(cap, net.w, net.h, net.c, &in_img[Global_id % 3], dont_close_stream);
    if(!in_s[Global_id%3].data){
        printf("Stream closed.\n");
        flag_exit = 1;
        //exit(EXIT_FAILURE);
        return 0;
    }
    //in_s = resize_image(in, net.w, net.h);

    in_img_resize[Global_id % 3] = img_resize(in_img[Global_id % 3]);
    Hist_img1[Global_id % 3] = HistEqual(in_img_resize[Global_id % 3]);
    GMM_img[Global_id % 3] = GMM_update(pGMM, in_img_resize[Global_id % 3]); //GMM+median
    thresh_GMM[Global_id % 3] = Count_Non_Zero(GMM_img[Global_id % 3]);
    
    
    return 0;
}

void *detect_in_thread(void *ptr)
{
    
    layer l = net.layers[net.n-1];
    float *X = det_s[Mid_id].data;
    
    float *prediction = network_predict(net, X);
    
    memcpy(predictions[demo_index], prediction, l.outputs*sizeof(float));
    mean_arrays(predictions, NFRAMES, l.outputs, avg);
    
    l.output = avg;

    free_image(det_s[Mid_id]);
    
    cv_images[demo_index] = det_img[Mid_id];
    det_img[Mid_id] = cv_images[(demo_index + NFRAMES / 2 + 1) % NFRAMES];
    demo_index = (demo_index + 1) % NFRAMES;
    
    if (letter_box)
        dets[Mid_id] = get_network_boxes(&net, get_width_mat(in_img[Mid_id]), get_height_mat(in_img[Mid_id]), demo_thresh, demo_thresh, 0, 1, &nboxes[Mid_id], 1); // letter box
    else
        dets[Mid_id] = get_network_boxes(&net, net.w, net.h, demo_thresh, demo_thresh, 0, 1, &nboxes[Mid_id], 0); // resized

    if (nms) do_nms_sort(dets[Mid_id], nboxes[Mid_id], l.classes, nms);
    
    return 0;
}

void *compute_size() {
    ///*Area*/
    area_img[Post_id] = threshold_otsu(in_img_resize[Post_id], 1); //�����̹����� ����ȭ, 1�� ��ü ��� 2�� ��ü ����
    area_img[Post_id] = openning(area_img[Post_id]);

    ///*Edge*/
    edge_img[Post_id] = edge(in_img_resize[Post_id], 1); //1. canny, 2. sobel + openning, ������ ���� ���

    ///*Merge */
    AND_img[Post_id] = AND_image(area_img[Post_id], edge_img[Post_id]); //��������� ���� �ռ�, �������� ����
    Merge_img[Post_id] = Merge_image(GMM_img[Post_id], area_img[Post_id]);
    pixel_counter(Merge_img[Post_id], dets[Post_id], nboxes[Post_id], demo_thresh, demo_names, demo_classes, count);
    
}



double get_wall_time()
{
    struct timeval walltime;
    if (gettimeofday(&walltime, NULL)) {
        return 0;
    }
    return (double)walltime.tv_sec + (double)walltime.tv_usec * .000001;
}

void demo(char *cfgfile, char *weightfile, float thresh, float hier_thresh, int cam_index, const char *filename, char **names, int classes,
    int frame_skip, char *prefix, char *out_filename, int mjpeg_port, int json_port, int dont_show, int ext_output, int letter_box_in)
{
    
    double ave_fps = 0.0;
    letter_box = letter_box_in;
    in_img[Global_id % 3] = det_img[Global_id % 3] = show_img[Global_id % 3] = NULL;
    //skip = frame_skip;
    image **alphabet = load_alphabet();
    int delay = frame_skip;
    demo_names = names;
    demo_alphabet[Global_id % 3] = alphabet;
    demo_classes = classes;
    demo_thresh = thresh;
    demo_ext_output[Global_id % 3] = ext_output;
    demo_json_port[Global_id % 3] = json_port;
    printf("Demo\n");
    net = parse_network_cfg_custom(cfgfile, 1, 1);    // set batch=1
    if(weightfile){
        load_weights(&net, weightfile);
    }
    fuse_conv_batchnorm(net);
    calculate_binary_weights(net);
    srand(2222222);
    
    if(filename){
        printf("video file: %s\n", filename);
        cap = get_capture_video_stream(filename);
    }else{
        printf("Webcam index: %d\n", cam_index);
        cap = get_capture_webcam(cam_index);
    }

    if (!cap) {
#ifdef WIN32
        printf("Check that you have copied file opencv_ffmpeg340_64.dll to the same directory where is darknet.exe \n");
#endif
        error("Couldn't connect to webcam.\n");
    }

    layer l = net.layers[net.n-1];
    int j;

    avg = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < NFRAMES; ++j) predictions[j] = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < NFRAMES; ++j) images[j] = make_image(1,1,3);

    if (l.classes != demo_classes) {
        printf("Parameters don't match: in cfg-file classes=%d, in data-file classes=%d \n", l.classes, demo_classes);
        getchar();
        exit(0);
    }
    
    pGMM = cvCreatePixelBackgroundGMM(416, 416);

    flag_exit = 0;

    pthread_t fetch_thread;
    pthread_t detect_thread;
    pthread_t compute_size_thread;
    pthread_t GMM_work_thread;

//    fetch_in_thread(0);
//    det_img = in_img;
//    det_s = in_s;

//    fetch_in_thread(0);
//    detect_in_thread(0);
//    det_img = in_img;
//    det_s = in_s;

//    for (j = 0; j < NFRAMES / 2; ++j) {
//        fetch_in_thread(0);
//        detect_in_thread(0);
//        det_img = in_img;
//        det_s = in_s;
//    }
    

    if(!prefix && !dont_show){
        int full_screen = 0;
        //create_window_cv("Demo", full_screen, 1352, 1013);
    }


    write_cv* output_video_writer = NULL;
    if (out_filename && !flag_exit)
    {
        int src_fps = 25;
        src_fps = get_stream_fps_cpp_cv(cap);
        output_video_writer =
            create_video_writer(out_filename, 'D', 'I', 'V', 'X', src_fps, get_width_mat(det_img), get_height_mat(det_img), 1);

        //'H', '2', '6', '4'
        //'D', 'I', 'V', 'X'
        //'M', 'J', 'P', 'G'
        //'M', 'P', '4', 'V'
        //'M', 'P', '4', '2'
        //'X', 'V', 'I', 'D'
        //'W', 'M', 'V', '2'
    }

    double before = get_wall_time();

    
    

    while(1){
        {
            //Pre_processing
            if (count <19 ) {
                if (pthread_create(&fetch_thread, 0, fetch_in_thread, 0)) error("Thread creation failed");
                pthread_join(fetch_thread, 0);
                Global_id++;
                count++;
            }

            if (count >= 19) {
                if (pthread_create(&fetch_thread, 0, fetch_in_thread, 0)) error("Thread creation failed");

                if (thresh_GMM[(Global_id - 1) % 3] > 3200) {
                    //Mid_processing
                    Mid_id = (Global_id - 1) % 3;
                    if (pthread_create(&detect_thread, 0, detect_in_thread, 0)) error("Thread creation failed");

                    Post_id = (Global_id - 1) % 3;
                    //Post_processing
                    if (pthread_create(&compute_size_thread, 0, compute_size, 0)) error("Thread creation failed");

                    pthread_join(compute_size_thread, 0);
                    pthread_join(detect_thread, 0);
                    
                }
                
                pthread_join(fetch_thread, 0);
                Global_id++;
                count++;


            }
            
        

            

            //For demo
            draw_detections_cv_v3(show_img[Global_id % 3], dets[Global_id % 3], nboxes[Global_id % 3], demo_thresh, demo_names, demo_alphabet, demo_classes, demo_ext_output);
            //free_detections(local_dets, local_nboxes);
            
            ave_fps += (double)fps;
            printf("\n Average FPS:%.1lf, %.1lf, %.1f, %d\n", ave_fps/count, ave_fps, fps, count);

            if(!prefix){
                if (!dont_show) {
                    //show_image_mat(show_img[Global_id % 3], "Demo");
                    //show_image_mat(in_img[Global_id % 3], "1. in_img_resize");
                    //show_image_mat(edge_img[Global_id % 3], "2. Edge_img");
                    //show_image_mat(area_img[Global_id % 3], "3. area_img");
                    //show_image_mat(AND_img[Global_id % 3], "4. AND_img");
                    //show_image_mat(GMM_img[Global_id % 3], "5. GMM_img");
                    show_image_mat(Merge_img[Global_id % 3], "6. Merge_img");
                    
                    int c = wait_key_cv(1);
                    if (c == 10) {
                        if (frame_skip == 0) frame_skip = 60;
                        else if (frame_skip == 4) frame_skip = 0;
                        else if (frame_skip == 60) frame_skip = 4;
                        else frame_skip = 0;
                    }
                    else if (c == 27 || c == 1048603) // ESC - exit (OpenCV 2.x / 3.x)
                    {
                        flag_exit = 1;
                    }
                }
            }else{
                char buff[256];
                sprintf(buff, "results/%s_%08d.jpg", prefix, count);
                if(show_img[Global_id % 3]) save_cv_jpg(show_img[Global_id % 3], buff);
            }

            // if you run it with param -mjpeg_port 8090  then open URL in your web-browser: http://localhost:8090
            if (mjpeg_port > 0 && show_img[Global_id % 3]) {
                int port = mjpeg_port;
                int timeout = 400000;
                int jpeg_quality = 40;    // 1 - 100
                send_mjpeg(show_img[Global_id % 3], port, timeout, jpeg_quality);
            }

            // save video file
            if (output_video_writer && show_img) {
                write_frame_cv(output_video_writer, show_img);
                printf("\n cvWriteFrame \n");
            }

            release_mat(&show_img[(Global_id-2) % 3]);
            release_mat(&in_img_resize[(Global_id-2) % 3]);
            release_mat(&Hist_img1[(Global_id-2) % 3]);
            release_mat(&GMM_img[(Global_id-2) % 3]);

            
            release_mat(&in_img_resize[(Global_id - 3) % 3]);
            release_mat(&area_img[(Global_id-3) % 3]);
            release_mat(&edge_img[(Global_id - 3) % 3]);
            release_mat(&AND_img[(Global_id - 3) % 3]);
            release_mat(&Merge_img[(Global_id - 3) % 3]);




            if (flag_exit == 1) break;

            if(delay == 0){
                show_img[Global_id % 3] = det_img[Global_id % 3];
            }
            det_img[Global_id % 3] = in_img[Global_id % 3];
            det_s[Global_id % 3] = in_s[Global_id % 3];
            
        }
        --delay;
        if(delay < 0){
            delay = frame_skip;

            //double after = get_wall_time();
            //float curr = 1./(after - before);
            double after = get_time_point();    // more accurate time measurements
            float curr = 1000000. / (after - before);
            fps = curr;
            before = after;
        }
    }
    printf("input video stream closed. \n");
    if (output_video_writer) {
        release_video_writer(&output_video_writer);
        printf("output_video_writer closed. \n");
    }

    // free memory
    release_mat(&show_img[Global_id % 3]);
    release_mat(&in_img[Global_id % 3]);
    free_image(in_s[Global_id % 3]);

    free(avg);
    for (j = 0; j < NFRAMES; ++j) free(predictions[j]);
    for (j = 0; j < NFRAMES; ++j) free_image(images[j]);

    free_ptrs((void **)names, net.layers[net.n - 1].classes);

    int i;
    const int nsize = 8;
    for (j = 0; j < nsize; ++j) {
        for (i = 32; i < 127; ++i) {
            free_image(alphabet[j][i]);
        }
        free(alphabet[j]);
    }
    free(alphabet);
    free_network(net);
    //cudaProfilerStop();
}
#else
void demo(char *cfgfile, char *weightfile, float thresh, float hier_thresh, int cam_index, const char *filename, char **names, int classes,
    int frame_skip, char *prefix, char *out_filename, int mjpeg_port, int json_port, int dont_show, int ext_output, int letter_box_in)
{
    fprintf(stderr, "Demo needs OpenCV for webcam images.\n");
}
#endif