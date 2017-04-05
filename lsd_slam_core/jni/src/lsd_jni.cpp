#include <string.h>
#include <jni.h>
#include <android/log.h>

#include "LiveSLAMWrapper.h"
#include <boost/thread.hpp>
#include "util/settings.h"
#include "util/Parse.h"
#include "util/globalFuncs.h"
#include "util/ThreadMutexObject.h"
#include "IOWrapper/Pangolin/PangolinOutput3DWrapper.h"
#include "SlamSystem.h"
#include <sstream>
#include <fstream>
#include <dirent.h>
#include <algorithm>
#include "util/Undistorter.h"
#include "util/RawLogReader.h"
#include "opencv2/opencv.hpp"
#include "GUI.h"
#include "util/logger.h"
#include "sophus/sim3.hpp"


// TODO: remove hard code
#define CALIB_FILE "/sdcard/LSD/cameraCalibration.cfg"
#define IMAGE_DIR "/sdcard/LSD/images"

using namespace lsd_slam;
ThreadMutexObject<bool> lsdDone(false);
std::vector<std::string> files;
int w, h, w_inp, h_inp;
RawLogReader * logReader = 0;
int numFrames = 0;
Undistorter* undistorter = NULL;
SlamSystem * slamSystem = NULL;
Sophus::Matrix3f K;


std::string &ltrim(std::string &s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
        return s;
}
std::string &rtrim(std::string &s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
        return s;
}
std::string &trim(std::string &s) {
        return ltrim(rtrim(s));
}
int getdir (std::string dir, std::vector<std::string> &files)
{
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(dir.c_str())) == NULL)
    {
        return -1;
    }

    while ((dirp = readdir(dp)) != NULL) {
    	std::string name = std::string(dirp->d_name);

    	if(name != "." && name != "..")
    		files.push_back(name);
    }
    closedir(dp);


    std::sort(files.begin(), files.end());

    if(dir.at( dir.length() - 1 ) != '/') dir = dir+"/";
	for(unsigned int i=0;i<files.size();i++)
	{
		if(files[i].at(0) != '/')
			files[i] = dir + files[i];
	}

    return files.size();
}

int getFile (std::string source, std::vector<std::string> &files)
{
	std::ifstream f(source.c_str());

	if(f.good() && f.is_open())
	{
		while(!f.eof())
		{
			std::string l;
			std::getline(f,l);

			l = trim(l);

			if(l == "" || l[0] == '#')
				continue;

			files.push_back(l);
		}

		f.close();

		size_t sp = source.find_last_of('/');
		std::string prefix;
		if(sp == std::string::npos)
			prefix = "";
		else
			prefix = source.substr(0,sp);

		for(unsigned int i=0;i<files.size();i++)
		{
			if(files[i].at(0) != '/')
				files[i] = prefix + "/" + files[i];
		}

		return (int)files.size();
	}
	else
	{
		f.close();
		return -1;
	}
}

void printMatrix(const Sophus::Sim3f::Transformation& trans) {
    std::ostringstream out;
    out << trans;
    LOGD("matrix:\n%s", out.str().c_str());
}

void run_once(SlamSystem * system, Undistorter* undistorter, Output3DWrapper* outputWrapper, Sophus::Matrix3f K)
{    
    
}

void run(SlamSystem * system, Undistorter* undistorter, Output3DWrapper* outputWrapper, Sophus::Matrix3f K) {
    LOGD("----------run-----------\n");
    // get HZ
    double hz = 30;

    cv::Mat image = cv::Mat(h, w, CV_8U);
    int runningIDX=0;
    float fakeTimeStamp = 0;

//    for(unsigned int i = 0; i < numFrames; i++)
    for(unsigned int i = 0; i < 5; i++)
    {
        if(lsdDone.getValue())
            break;

        cv::Mat imageDist = cv::Mat(h, w, CV_8U);

        if(logReader)
        {
            logReader->getNext();

            cv::Mat3b img(h, w, (cv::Vec3b *)logReader->rgb);

            cv::cvtColor(img, imageDist, CV_RGB2GRAY);
        }
        else
        {
            imageDist = cv::imread(files[i], CV_LOAD_IMAGE_GRAYSCALE);

            if(imageDist.rows != h_inp || imageDist.cols != w_inp)
            {
                if(imageDist.rows * imageDist.cols == 0)
                    printf("failed to load image %s! skipping.\n", files[i].c_str());
                else
                    printf("image %s has wrong dimensions - expecting %d x %d, found %d x %d. Skipping.\n",
                            files[i].c_str(),
                            w,h,imageDist.cols, imageDist.rows);
                continue;
            }
        }

        assert(imageDist.type() == CV_8U);

        undistorter->undistort(imageDist, image);

        assert(image.type() == CV_8U);
        if(runningIDX == 0)
        {
            system->randomInit(image.data, fakeTimeStamp, runningIDX);
        }
        else
        {
            system->trackFrame(image.data, runningIDX, hz == 0, fakeTimeStamp);
        }
        
        printMatrix(system->getCurrentPoseEstimateScale().matrix());
        //gui.pose.assignValue(system->getCurrentPoseEstimateScale());

        runningIDX++;
        fakeTimeStamp+=0.03;
 
        if(fullResetRequested)
        {
            LOGD("FULL RESET!\n");
            //delete system;

            //system = new SlamSystem(w, h, K, doSlam);
            //system->setVisualization(outputWrapper);

            fullResetRequested = false;
            runningIDX = 0;
        }
    }
    
    
}


extern "C"{
JavaVM* jvm = NULL;

//init LSD
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeInit(JNIEnv* env, jobject thiz) {
	LOGD("nativeInit");
    //init jni
	env->GetJavaVM(&jvm);
	
	std::string calibFile = CALIB_FILE;
	undistorter = Undistorter::getUndistorterForFile(calibFile.c_str());
	if(undistorter == 0) {
		LOGE("need camera calibration file! (set using -c FILE)\n");
		exit(0);
	}

	w = undistorter->getOutputWidth();
	h = undistorter->getOutputHeight();

	w_inp = undistorter->getInputWidth();
	h_inp = undistorter->getInputHeight();
	LOGD("w=%d, h=%d, w_inp=%d, h_inp=%d\n", w, h, w_inp, h_inp);

//   GUI gui;
	float fx = undistorter->getK().at<double>(0, 0);
	float fy = undistorter->getK().at<double>(1, 1);
	float cx = undistorter->getK().at<double>(2, 0);
	float cy = undistorter->getK().at<double>(2, 1);
	
	K << fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0;
	LOGD("fx=%f, fy=%f, cx=%f, cy=%f\n", fx, fy, cx, cy);

	Resolution::getInstance(w, h);
	Intrinsics::getInstance(fx, fy, cx, cy);

//	gui.initImages();
//	Output3DWrapper* outputWrapper = new PangolinOutput3DWrapper(w, h, gui);

	// make slam system
	slamSystem = new SlamSystem(w, h, K, doSlam);
//	slamSystem->setVisualization(outputWrapper);

    // open image files: first try to open as file.
	std::string source = IMAGE_DIR;

	Bytef * decompressionBuffer = new Bytef[Resolution::getInstance().numPixels() * 2];
    IplImage * deCompImage = 0;

    if(source.substr(source.find_last_of(".") + 1) == "klg") {
        logReader = new RawLogReader(decompressionBuffer,
                                     deCompImage,
                                     source);
        numFrames = logReader->getNumFrames();
    }
    else {
        if(getdir(source, files) >= 0) {
            LOGD("found %d image files in folder %s!\n", (int)files.size(), source.c_str());
        }
        else if(getFile(source, files) >= 0) {
            LOGD("found %d image files in file %s!\n", (int)files.size(), source.c_str());
        }
        else {
            LOGD("could not load file list! wrong path / file?\n");
        }
        numFrames = (int)files.size();
    }
}

// clean up
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeDestroy(JNIEnv* env, jobject thiz) {
	LOGD("nativeDestroy");
	lsdDone.assignValue(true);
}

// init OpenGL
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeInitGL(JNIEnv* env, jobject thiz) {
	LOGD("nativeInitGL");
	boost::thread lsdThread(run, slamSystem, undistorter, (Output3DWrapper* )NULL, K);    
}

//resize window (might only work once)
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeResize(JNIEnv* env, jobject thiz , jint w, jint h) {
	LOGD("nativeResize: w=%d, h=%d\n", w, h);
}

//render and process a new frame
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeRender(JNIEnv* env, jobject thiz) {
//    LOGD("nativeRender");
    
    //run_once(slamSystem, undistorter, NULL, K);
}

//forward keyboard to LSD
JNIEXPORT void JNICALL
Java_com_tc_tar_TARNativeInterface_nativeKey(JNIEnv* env, jobject thiz, jint keycode) {
    LOGD("nativeKey: keycode=%d\n", keycode);
}


}