#include "gif.h"

/**
 * Generates default color map, used when there is no color map defined in GIF file
 */
static ColorMapObject* genDefColorMap(void);

/**
 * @return the real time, in ms
 */
static __time_t getRealTime(void);

/**
 * Frees dynamically allocated memory
 */
static void cleanUp(GifInfo* info);

static JavaVM* g_jvm;
static ColorMapObject* defaultCmap = NULL;

static ColorMapObject* genDefColorMap(void)
{
	ColorMapObject* cmap = GifMakeMapObject(256, NULL);
	if (cmap != NULL)
	{
		int iColor;
		for (iColor = 0; iColor < 256; iColor++)
		{
			cmap->Colors[iColor].Red = (GifByteType) iColor;
			cmap->Colors[iColor].Green = (GifByteType) iColor;
			cmap->Colors[iColor].Blue = (GifByteType) iColor;
		}
	}
	return cmap;
}

static void cleanUp(GifInfo* info)
{
	free(info->backupPtr);
	info->backupPtr = NULL;
	free(info->infos);
	info->infos = NULL;
	free(info->rasterBits);
	info->rasterBits = NULL;
	free(info->comment);
	info->comment = NULL;

	GifFileType* GifFile = info->gifFilePtr;
	if (GifFile->SColorMap == defaultCmap)
		GifFile->SColorMap = NULL;
	if (GifFile->SavedImages != NULL)
	{
		SavedImage* sp;
		for (sp = GifFile->SavedImages;
				sp < GifFile->SavedImages + GifFile->ImageCount; sp++)
		{
			if (sp->ImageDesc.ColorMap != NULL)
			{
				GifFreeMapObject(sp->ImageDesc.ColorMap);
				sp->ImageDesc.ColorMap = NULL;
			}
		}
		free(GifFile->SavedImages);
		GifFile->SavedImages = NULL;
	}
	DGifCloseFile(GifFile);
	free(info);
}

static __time_t getRealTime(void)
{
	struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != -1)
		return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	return -1;
}

static int fileRead(GifFileType *gif, GifByteType *bytes, int size)
{
	FILE* file = (FILE*) gif->UserData;
	return (int) fread(bytes, 1, (size_t) size, file);
}

static JNIEnv *getEnv(void)
{
	JNIEnv* env = NULL;
    (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
	return env;
}

static int directByteBufferReadFun(GifFileType* gif, GifByteType* bytes, int size)
{
	DirectByteBufferContainer* dbbc = gif->UserData;
	if (dbbc->pos + size > dbbc->capacity)
		size -= dbbc->pos + size - dbbc->capacity;
	memcpy(bytes, dbbc->bytes + dbbc->pos, (size_t) size);
	dbbc->pos += size;
	return size;
}

static int byteArrayReadFun(GifFileType* gif, GifByteType* bytes, int size)
{
	ByteArrayContainer* bac = gif->UserData;
	JNIEnv* env = NULL;
	(*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
	if (bac->pos + size > bac->arrLen)
		size -= bac->pos + size - bac->arrLen;
	(*env)->GetByteArrayRegion(env, bac->buffer, (jsize) bac->pos, size, (jbyte *) bytes);
	bac->pos += size;
	return size;
}

static int streamReadFun(GifFileType* gif, GifByteType* bytes, int size)
{
	StreamContainer* sc = gif->UserData;
	JNIEnv* env = getEnv();

	(*env)->MonitorEnter(env, sc->stream);

	if (sc->buffer == NULL)
	{
		jbyteArray buffer = (*env)->NewByteArray(env, size < 256 ? 256 : size);
		sc->buffer = (*env)->NewGlobalRef(env, buffer);
	}
	else
	{
		jsize bufLen = (*env)->GetArrayLength(env, sc->buffer);
		if (bufLen < size)
		{
			(*env)->DeleteGlobalRef(env, sc->buffer);
			sc->buffer = NULL;

			jbyteArray buffer = (*env)->NewByteArray(env, size);
			sc->buffer = (*env)->NewGlobalRef(env, buffer);
		}
	}

	int len = (*env)->CallIntMethod(env, sc->stream, sc->readMID, sc->buffer, 0, size);
	if ((*env)->ExceptionOccurred(env))
	{
		(*env)->ExceptionClear(env);
		len = 0;
	}
	else if (len > 0)
	{
		(*env)->GetByteArrayRegion(env, sc->buffer, 0, len, (jbyte *) bytes);
	}

	(*env)->MonitorExit(env, sc->stream);

	return len >= 0 ? len : 0;
}

static int fileRewind(GifInfo *info)
{
	return fseek(info->gifFilePtr->UserData, info->startPos, SEEK_SET);
}

static int streamRewind(GifInfo *info)
{
	GifFileType* gif = info->gifFilePtr;
	StreamContainer* sc = gif->UserData;
	JNIEnv* env = getEnv();
	(*env)->CallVoidMethod(env, sc->stream, sc->resetMID);
	if ((*env)->ExceptionOccurred(env))
	{
		(*env)->ExceptionClear(env);
		return -1;
	}
	return 0;
}

static int byteArrayRewind(GifInfo *info)
{
	GifFileType* gif = info->gifFilePtr;
	ByteArrayContainer* bac = gif->UserData;
	bac->pos = info->startPos;
	return 0;
}

static int directByteBufferRewindFun(GifInfo* info)
{
	GifFileType* gif = info->gifFilePtr;
	DirectByteBufferContainer* dbbc = gif->UserData;
	dbbc->pos = info->startPos;
	return 0;
}

static int getComment(GifByteType* Bytes, char** cmt)
{
	unsigned int len = (unsigned int) Bytes[0];
	size_t offset = *cmt != NULL ? strlen(*cmt) : 0;
	char* ret = realloc(*cmt, (len + offset + 1) * sizeof(char));
	if (ret != NULL)
	{
		memcpy(ret + offset, &Bytes[1], len);
		ret[len + offset] = 0;
		*cmt = ret;
		return GIF_OK;
	}
	return GIF_ERROR;
}

static void packARGB32(argb* pixel, GifByteType alpha, GifByteType red,
		GifByteType green, GifByteType blue)
{
	pixel->alpha = alpha;
	pixel->red = red;
	pixel->green = green;
	pixel->blue = blue;
}

static void getColorFromTable(int idx, argb* dst, const ColorMapObject* cmap)
{
	int colIdx = (idx >= cmap->ColorCount) ? 0 : idx;
	GifColorType* col = &cmap->Colors[colIdx];
	packARGB32(dst, 0xFF, col->Red, col->Green,col->Blue);
}

static void eraseColor(argb* bm, int w, int h, argb color)
{
	int i;
	for (i = 0; i < w * h; i++)
		*(bm + i) = color;
}

static inline bool setupBackupBmp(GifInfo* info, int transpIndex)
{
	GifFileType* fGIF = info->gifFilePtr;
	info->backupPtr = calloc((size_t) (fGIF->SWidth * fGIF->SHeight), sizeof(argb));
	if (!info->backupPtr)
	{
		info->gifFilePtr->Error = D_GIF_ERR_NOT_ENOUGH_MEM;
		return false;
	}
	argb paintingColor;
	if (transpIndex == -1)
		getColorFromTable(fGIF->SBackGroundColor, &paintingColor,
				fGIF->SColorMap);
	else
		packARGB32(&paintingColor, 0, 0, 0, 0);
	eraseColor(info->backupPtr, fGIF->SWidth, fGIF->SHeight, paintingColor);
	return true;
}

static int readExtensions(int ExtFunction, GifByteType* ExtData, GifInfo* info)
{
	if (ExtData == NULL)
		return GIF_OK;
	if (ExtFunction == GRAPHICS_EXT_FUNC_CODE)
	{
        GraphicsControlBlock GCB;
        if (DGifExtensionToGCB(ExtData[0],  ExtData+1,   &GCB) == GIF_ERROR)
            return GIF_ERROR;

        FrameInfo* fi = &info->infos[info->gifFilePtr->ImageCount];
        fi->disposalMethod= (unsigned char) GCB.DisposalMode;
        fi->duration= GCB.DelayTime> 1 ? (unsigned int)GCB.DelayTime * 10 : 100;
        fi->transpIndex=GCB.TransparentColor;

        if (fi->disposalMethod == DISPOSE_PREVIOUS && info->backupPtr == NULL)
        {
            if (!setupBackupBmp(info, fi->transpIndex))
                return GIF_ERROR;
        }
	}
	else if (ExtFunction == COMMENT_EXT_FUNC_CODE)
	{
		if (getComment(ExtData, &info->comment) == GIF_ERROR)
		{
			info->gifFilePtr->Error = D_GIF_ERR_NOT_ENOUGH_MEM;
			return GIF_ERROR;
		}
	}
	else if (ExtFunction == APPLICATION_EXT_FUNC_CODE && ExtData[0] == 11)
	{
		if (strcmp("NETSCAPE2.0", ExtData+1) == 0
				|| strcmp("ANIMEXTS1.0", ExtData+1) == 0)
		{
			if (DGifGetExtensionNext(info->gifFilePtr, &ExtData,
					&ExtFunction)==GIF_ERROR)
				return GIF_ERROR;
			if (ExtData[0] == 3
					&& ExtData[1] == 1)
			{
				info->loopCount = (unsigned short) (ExtData[2]
						+ (ExtData[3] << 8));
				if (info->loopCount > 0)
			        info->currentLoop = 0;
			}
		}
	}
	return GIF_OK;
}

static int DDGifSlurp(GifFileType* GifFile, GifInfo* info, bool shouldDecode)
{
	GifRecordType RecordType;
	GifByteType* ExtData;
	int codeSize;
	int ExtFunction;
	int ImageSize;
	do
	{
		if (DGifGetRecordType(GifFile, &RecordType) == GIF_ERROR)
			return (GIF_ERROR);
		switch (RecordType)
		{
		case IMAGE_DESC_RECORD_TYPE:

			if (DGifGetImageDesc(GifFile, !shouldDecode) == GIF_ERROR)
				return (GIF_ERROR);
			SavedImage* sp = &GifFile->SavedImages[(shouldDecode ? info->currentIndex : GifFile->ImageCount - 1)];
			ImageSize = sp->ImageDesc.Width * sp->ImageDesc.Height;

			if (sp->ImageDesc.Width < 1 || sp->ImageDesc.Height < 1
					|| ImageSize > (SIZE_MAX / sizeof(GifPixelType)))
			{
				GifFile->Error = D_GIF_ERR_INVALID_IMG_DIMS;
				return GIF_ERROR;
			}
			if (sp->ImageDesc.Width > GifFile->SWidth
					|| sp->ImageDesc.Height > GifFile->SHeight)
			{
				GifFile->Error = D_GIF_ERR_IMG_NOT_CONFINED;
				return GIF_ERROR;
			}
			if (shouldDecode)
			{

				sp->RasterBits = info->rasterBits;

				if (sp->ImageDesc.Interlace)
				{
					int i, j;
					/*
					 * The way an interlaced image should be read -
					 * offsets and jumps...
					 */
					int InterlacedOffset[] =
					{ 0, 4, 2, 1 };
					int InterlacedJumps[] =
					{ 8, 8, 4, 2 };
					/* Need to perform 4 passes on the image */
					for (i = 0; i < 4; i++)
						for (j = InterlacedOffset[i]; j < sp->ImageDesc.Height;
								j += InterlacedJumps[i])
						{
							if (DGifGetLine(GifFile,
									sp->RasterBits + j * sp->ImageDesc.Width,
									sp->ImageDesc.Width) == GIF_ERROR)
								return GIF_ERROR;
						}
				}
				else
				{
					if (DGifGetLine(GifFile, sp->RasterBits,
							ImageSize) == GIF_ERROR)
						return (GIF_ERROR);
				}
				if (info->currentIndex >= GifFile->ImageCount - 1)
				{
					if (info->loopCount > 0)
						info->currentLoop++;
					if (info->rewindFunction(info) != 0)
					{
						info->gifFilePtr->Error = D_GIF_ERR_REWIND_FAILED;
						return GIF_ERROR;
					}
				}
				return GIF_OK;
			}
			else
			{
				if (DGifGetCode(GifFile, &codeSize, &ExtData) == GIF_ERROR)
					return (GIF_ERROR);
				while (ExtData != NULL)
				{
					if (DGifGetCodeNext(GifFile, &ExtData) == GIF_ERROR)
						return (GIF_ERROR);
				}
			}
			break;

		case EXTENSION_RECORD_TYPE:
			if (DGifGetExtension(GifFile, &ExtFunction, &ExtData) == GIF_ERROR)
				return (GIF_ERROR);

			if (!shouldDecode)
			{
				FrameInfo* tmpInfos = realloc(info->infos,
						(GifFile->ImageCount + 1) * sizeof(FrameInfo));
                if (tmpInfos==NULL)
                    return GIF_ERROR;
                info->infos=tmpInfos;
				if (readExtensions(ExtFunction, ExtData, info) == GIF_ERROR)
					return GIF_ERROR;
			}
			while (ExtData != NULL)
			{
				if (DGifGetExtensionNext(GifFile, &ExtData,
						&ExtFunction) == GIF_ERROR)
					return (GIF_ERROR);
				if (!shouldDecode)
				{
					if (readExtensions(ExtFunction, ExtData, info) == GIF_ERROR)
						return GIF_ERROR;
				}
			}
			break;

		case TERMINATE_RECORD_TYPE:
			break;

		default: /* Should be trapped by DGifGetRecordType */
			break;
		}
	} while (RecordType != TERMINATE_RECORD_TYPE);
	bool ok = true;
	if (shouldDecode)
	{
		ok = (info->rewindFunction(info) == 0);
	}
	if (ok)
		return (GIF_OK);
	else
	{
		info->gifFilePtr->Error = D_GIF_ERR_READ_FAILED;
		return (GIF_ERROR);
	}
}

static void setMetaData(int width, int height, int ImageCount, int errorCode,
		JNIEnv * env, jintArray metaData)
{
	jint* const ints = (*env)->GetIntArrayElements(env, metaData, 0);
	if (ints==NULL)
	    return;
	ints[0] = width;
	ints[1] = height;
	ints[2] = ImageCount;
	ints[3] = errorCode;
	(*env)->ReleaseIntArrayElements(env, metaData, ints, 0);
	if (errorCode == 0)
		return;

	jclass exClass = (*env)->FindClass(env,
			"pl/droidsonroids/gif/GifIOException");

	if (exClass == NULL)
		return;
	jmethodID mid = (*env)->GetMethodID(env, exClass, "<init>", "(I)V");
	if (mid == NULL)
		return;
	jobject exception = (*env)->NewObject(env, exClass, mid, errorCode);
	if (exception != NULL)
		(*env)->Throw(env, exception);
}

static GifInfo* open(GifFileType* GifFileIn, int Error, long startPos,
		RewindFunc rewindFunc, JNIEnv * env, jintArray metaData, const jboolean justDecodeMetaData)
{
	if (startPos < 0)
	{
		Error = D_GIF_ERR_NOT_READABLE;
		DGifCloseFile(GifFileIn);
	}
	if (Error != 0 || GifFileIn == NULL)
	{
		setMetaData(0, 0, 0, Error, env, metaData);
		return NULL;
	}
	int width = GifFileIn->SWidth, height = GifFileIn->SHeight;
	int wxh = width * height;
	if (wxh < 1 || wxh > INT_MAX)
	{
		DGifCloseFile(GifFileIn);
		setMetaData(width, height, 0,
		D_GIF_ERR_INVALID_SCR_DIMS, env, metaData);
		return NULL;
	}
	GifInfo* info = malloc(sizeof(GifInfo));
	if (info == NULL)
	{
		DGifCloseFile(GifFileIn);
		setMetaData(width, height, 0,
		D_GIF_ERR_NOT_ENOUGH_MEM, env, metaData);
		return NULL;
	}
	info->gifFilePtr = GifFileIn;
	info->startPos = startPos;
	info->currentIndex = -1;
	info->nextStartTime = 0;
	info->lastFrameReaminder = ULONG_MAX;
	info->comment = NULL;
	info->loopCount = 0;
	info->currentLoop = -1;
	info->speedFactor = 1.0;
	if (justDecodeMetaData == JNI_TRUE)
	    info->rasterBits=NULL;
	else
	    info->rasterBits = calloc((size_t) (GifFileIn->SHeight * GifFileIn->SWidth),
			sizeof(GifPixelType));
	info->infos = malloc(sizeof(FrameInfo));
	info->backupPtr = NULL;
	info->rewindFunction = rewindFunc;

	if ((info->rasterBits == NULL && justDecodeMetaData != JNI_TRUE) || info->infos == NULL)
	{
		cleanUp(info);
		setMetaData(width, height, 0,
		D_GIF_ERR_NOT_ENOUGH_MEM, env, metaData);
		return NULL;
	}
	info->infos->duration = 0;
	info->infos->disposalMethod = DISPOSAL_UNSPECIFIED;
	info->infos->transpIndex = NO_TRANSPARENT_COLOR;
	if (GifFileIn->SColorMap == NULL
			|| GifFileIn->SColorMap->ColorCount
					!= (1 << GifFileIn->SColorMap->BitsPerPixel))
	{
		GifFreeMapObject(GifFileIn->SColorMap);
		GifFileIn->SColorMap = defaultCmap;
	}

#if defined(STRICT_FORMAT_89A)
	if (DDGifSlurp(GifFileIn, info, false) == GIF_ERROR)
		Error = GifFileIn->Error;
#else
	DDGifSlurp(GifFileIn, info, false);
#endif

	int imgCount = GifFileIn->ImageCount;

	if (imgCount < 1)
		Error = D_GIF_ERR_NO_FRAMES;
	if (info->rewindFunction(info) != 0)
		Error = D_GIF_ERR_READ_FAILED;
	if (Error != 0)
		cleanUp(info);
	setMetaData(width, height, imgCount, Error, env, metaData);

	return Error == 0 ? info : NULL;
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_openFile(JNIEnv * env, jclass class,
		jintArray metaData, jstring jfname, jboolean justDecodeMetaData)
{
	if (jfname == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}

	const char * const fname = (*env)->GetStringUTFChars(env, jfname, 0);
	FILE * file = fopen(fname, "rb");
	(*env)->ReleaseStringUTFChars(env, jfname, fname);
	if (file == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	int Error = 0;
	GifFileType* GifFileIn = DGifOpen(file, &fileRead, &Error);
	return (jlong)(intptr_t) open(GifFileIn, Error, ftell(file), fileRewind, env, metaData, justDecodeMetaData);
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_openByteArray(JNIEnv * env, jclass class,
		jintArray metaData, jbyteArray bytes, jboolean justDecodeMetaData)
{
	ByteArrayContainer* container = malloc(sizeof(ByteArrayContainer));
	if (container == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_NOT_ENOUGH_MEM, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	container->buffer = (*env)->NewGlobalRef(env, bytes);
	container->arrLen = (*env)->GetArrayLength(env, container->buffer);
	container->pos = 0;
	int Error = 0;
	GifFileType* GifFileIn = DGifOpen(container, &byteArrayReadFun, &Error);

	GifInfo* openResult = open(GifFileIn, Error, container->pos, byteArrayRewind,
            env, metaData, justDecodeMetaData);

	if (openResult == NULL)
	{
		(*env)->DeleteGlobalRef(env, container->buffer);
		free(container);
		container=NULL;
	}
	return (jlong)(intptr_t) openResult;
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_openDirectByteBuffer(JNIEnv * env,
		jclass class, jintArray metaData, jobject buffer, jboolean justDecodeMetaData)
{
	jbyte* bytes = (*env)->GetDirectBufferAddress(env, buffer);
	jlong capacity = (*env)->GetDirectBufferCapacity(env, buffer);
	if (bytes == NULL || capacity <= 0)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	DirectByteBufferContainer* container = malloc(
			sizeof(DirectByteBufferContainer));
	if (container == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_NOT_ENOUGH_MEM, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	container->bytes = bytes;
	container->capacity = capacity;
	container->pos = 0;
	int Error = 0;
	GifFileType* GifFileIn = DGifOpen(container, &directByteBufferReadFun,
			&Error);

	GifInfo* openResult = open(GifFileIn, Error, container->pos,
			directByteBufferRewindFun, env, metaData, justDecodeMetaData);

	if (openResult == NULL)
	{
		free(container);
		container=NULL;
    }
	return (jlong)(intptr_t) openResult;
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_openStream(JNIEnv * env, jclass class,
		jintArray metaData, jobject stream, jboolean justDecodeMetaData)
{
	jclass streamCls = (*env)->NewGlobalRef(env,
			(*env)->GetObjectClass(env, stream));
	jmethodID mid = (*env)->GetMethodID(env, streamCls, "mark", "(I)V");
	jmethodID readMID = (*env)->GetMethodID(env, streamCls, "read", "([BII)I");
	jmethodID resetMID = (*env)->GetMethodID(env, streamCls, "reset", "()V");

	if (mid == 0 || readMID == 0 || resetMID == 0)
	{
		(*env)->DeleteGlobalRef(env, streamCls);
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}

	StreamContainer* container = malloc(sizeof(StreamContainer));
	if (container == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_NOT_ENOUGH_MEM, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	container->readMID = readMID;
	container->resetMID = resetMID;

	container->stream = (*env)->NewGlobalRef(env, stream);
	container->streamCls = streamCls;
	container->buffer = NULL;

	int Error = 0;
	GifFileType* GifFileIn = DGifOpen(container, &streamReadFun, &Error);

	(*env)->CallVoidMethod(env, stream, mid, LONG_MAX); //TODO better length?

	GifInfo* openResult = open(GifFileIn, Error, 0, streamRewind, env, metaData, justDecodeMetaData);
	if (openResult == NULL)
	{
		(*env)->DeleteGlobalRef(env, streamCls);
		(*env)->DeleteGlobalRef(env, container->stream);
		free(container);
		container=NULL;
	}
	return (jlong)(intptr_t) openResult;
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_openFd(JNIEnv * env, jclass class,
		jintArray metaData, jobject jfd, jlong offset, jboolean justDecodeMetaData)
{
	jclass fdClass = (*env)->GetObjectClass(env, jfd);
	jfieldID fdClassDescriptorFieldID = (*env)->GetFieldID(env, fdClass,
			"descriptor", "I");
	if (fdClassDescriptorFieldID == NULL)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}
	jint fd = (*env)->GetIntField(env, jfd, fdClassDescriptorFieldID);
	FILE* file = fdopen(dup(fd), "rb");
	if (file == NULL || fseek(file, offset, SEEK_SET) != 0)
	{
		setMetaData(0, 0, 0,
		D_GIF_ERR_OPEN_FAILED, env, metaData);
		return (jlong)(intptr_t) NULL;
	}

	int Error = 0;
	GifFileType* GifFileIn = DGifOpen(file, &fileRead, &Error);
	long startPos = ftell(file);

	return (jlong)(intptr_t) open(GifFileIn, Error, startPos, fileRewind, env, metaData, justDecodeMetaData);
}

static void copyLine(argb* dst, const unsigned char* src,
		const ColorMapObject* cmap, int transparent, int width)
{
	for (; width > 0; width--, src++, dst++)
	{
		if (*src != transparent)
			getColorFromTable(*src, dst, cmap);
	}
}

static argb*
getAddr(argb* bm, int width, int left, int top)
{
	return bm + top * width + left;
}

static void blitNormal(argb* bm, int width, int height, const SavedImage* frame,
		const ColorMapObject* cmap, int transparent)
{
	const unsigned char* src = frame->RasterBits;
	argb* dst = getAddr(bm, width, frame->ImageDesc.Left, frame->ImageDesc.Top);
	GifWord copyWidth = frame->ImageDesc.Width;
	if (frame->ImageDesc.Left + copyWidth > width)
	{
		copyWidth = width - frame->ImageDesc.Left;
	}

	GifWord copyHeight = frame->ImageDesc.Height;
	if (frame->ImageDesc.Top + copyHeight > height)
	{
		copyHeight = height - frame->ImageDesc.Top;
	}

	for (; copyHeight > 0; copyHeight--)
	{
		copyLine(dst, src, cmap, transparent, copyWidth);
		src += frame->ImageDesc.Width;
		dst += width;
	}
}

static void fillRect(argb* bm, int bmWidth, int bmHeight, GifWord left,
		GifWord top, GifWord width, GifWord height, argb col)
{
	uint32_t* dst = (uint32_t*) getAddr(bm, bmWidth, left, top);
	GifWord copyWidth = width;
	if (left + copyWidth > bmWidth)
	{
		copyWidth = bmWidth - left;
	}

	GifWord copyHeight = height;
	if (top + copyHeight > bmHeight)
	{
		copyHeight = bmHeight - top;
	}
	uint32_t* pColor = (uint32_t*) (&col);
	for (; copyHeight > 0; copyHeight--)
	{
		memset(dst, *pColor, copyWidth * sizeof(argb));
		dst += bmWidth;
	}
}

static void drawFrame(argb* bm, int bmWidth, int bmHeight,
		const SavedImage* frame, const ColorMapObject* cmap, int transpIndex)
{

	if (frame->ImageDesc.ColorMap != NULL)
	{
		// use local color table
		cmap = frame->ImageDesc.ColorMap;
		if (cmap->ColorCount != (1 << cmap->BitsPerPixel))
			cmap = defaultCmap;
	}

	blitNormal(bm, bmWidth, bmHeight, frame, cmap, transpIndex);
}

// return true if area of 'target' is completely covers area of 'covered'
static bool checkIfCover(const SavedImage* target, const SavedImage* covered)
{
	if (target->ImageDesc.Left <= covered->ImageDesc.Left
			&& covered->ImageDesc.Left + covered->ImageDesc.Width
					<= target->ImageDesc.Left + target->ImageDesc.Width
			&& target->ImageDesc.Top <= covered->ImageDesc.Top
			&& covered->ImageDesc.Top + covered->ImageDesc.Height
					<= target->ImageDesc.Top + target->ImageDesc.Height)
	{
		return true;
	}
	return false;
}

static inline void disposeFrameIfNeeded(argb* bm, GifInfo* info,
		int idx)
{
	argb* backup = info->backupPtr;
	argb color;
	packARGB32(&color, 0, 0, 0, 0);
	GifFileType* fGif = info->gifFilePtr;
	SavedImage* cur = &fGif->SavedImages[idx - 1];
	SavedImage* next = &fGif->SavedImages[idx];
	// We can skip disposal process if next frame is not transparent
	// and completely covers current area
    unsigned char curDisposal = info->infos[idx - 1].disposalMethod;
	bool nextTrans = info->infos[idx].transpIndex != NO_TRANSPARENT_COLOR;
    unsigned char nextDisposal = info->infos[idx].disposalMethod;
	if (nextTrans || !checkIfCover(next, cur))
	{
		if (curDisposal == DISPOSE_BACKGROUND)
		{// restore to background (under this image) color
			fillRect(bm, fGif->SWidth, fGif->SHeight, cur->ImageDesc.Left,
					cur->ImageDesc.Top, cur->ImageDesc.Width,
					cur->ImageDesc.Height, color);
        }
		else if (curDisposal == DISPOSE_PREVIOUS && nextDisposal == DISPOSE_PREVIOUS)
		{// restore to previous
			argb* tmp = bm;
			bm = backup;
			backup = tmp;
	    }
	}

	// Save current image if next frame's disposal method == DISPOSE_PREVIOUS
	if (nextDisposal == DISPOSE_PREVIOUS)
		memcpy(backup, bm, fGif->SWidth * fGif->SHeight * sizeof(argb));
}

static bool reset(GifInfo* info)
{
	if (info->rewindFunction(info) != 0)
		return false;
	info->nextStartTime = 0;
	info->currentLoop = -1;
	info->currentIndex = -1;
	info->lastFrameReaminder = ULONG_MAX;
	return true;
}

static void getBitmap(argb* bm, GifInfo* info)
{
	GifFileType* fGIF = info->gifFilePtr;
    if (fGIF->Error == D_GIF_ERR_REWIND_FAILED)
        return;

	int i = info->currentIndex;

	if (DDGifSlurp(fGIF, info, true) == GIF_ERROR)
	{
	    if (!reset(info))
	        fGIF->Error = D_GIF_ERR_REWIND_FAILED;
		return;
    }

	SavedImage* cur = &fGIF->SavedImages[i];
	int transpIndex = info->infos[i].transpIndex;
	if (i == 0)
	{
	    argb paintingColor;
		if (transpIndex == -1)
			getColorFromTable(fGIF->SBackGroundColor, &paintingColor,
					fGIF->SColorMap);
		else
			packARGB32(&paintingColor, 0, 0, 0, 0);
		eraseColor(bm, fGIF->SWidth, fGIF->SHeight, paintingColor);
	}
	else
	{
		// Dispose previous frame before move to next frame.
		disposeFrameIfNeeded(bm, info, i);
	}
	drawFrame(bm, fGIF->SWidth, fGIF->SHeight, cur, fGIF->SColorMap,
			transpIndex);
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_reset(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return;
	reset(info);
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_setSpeedFactor(JNIEnv * env, jclass class,
		jlong gifInfo, jfloat factor)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return;
	info->speedFactor = factor;
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_seekToTime(JNIEnv * env, jclass class,
		jlong gifInfo, jint desiredPos, jintArray jPixels)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL ||jPixels==NULL)
		return;
	int imgCount = info->gifFilePtr->ImageCount;
	if (imgCount <= 1)
		return;

	unsigned long sum = 0;
	int i;
	for (i = 0; i < imgCount; i++)
	{
		unsigned long newSum = sum + info->infos[i].duration;
		if (newSum >= desiredPos)
			break;
		sum = newSum;
	}
	if (i < info->currentIndex)
		return;

	unsigned long lastFrameRemainder = desiredPos - sum;
	if (i == imgCount - 1 && lastFrameRemainder > info->infos[i].duration)
		lastFrameRemainder = info->infos[i].duration;
	if (i > info->currentIndex)
	{
		jint* const pixels = (*env)->GetIntArrayElements(env, jPixels, 0);
		if (pixels==NULL)
		    return;
		while (info->currentIndex <= i)
		{
			info->currentIndex++;
            getBitmap((argb *) pixels, info);
		}
		(*env)->ReleaseIntArrayElements(env, jPixels, pixels, 0);
	}
	info->lastFrameReaminder = lastFrameRemainder;

	if (info->speedFactor == 1.0)
		info->nextStartTime = getRealTime() + lastFrameRemainder;
	else
		info->nextStartTime = getRealTime()
				+ (unsigned long)(lastFrameRemainder * info->speedFactor);
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_seekToFrame(JNIEnv * env, jclass class,
		jlong gifInfo, jint desiredIdx, jintArray jPixels)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL|| jPixels==NULL)
		return;
	if (desiredIdx <= info->currentIndex)
		return;

	int imgCount = info->gifFilePtr->ImageCount;
	if (imgCount <= 1)
		return;

	jint * const pixels = (*env)->GetIntArrayElements(env, jPixels, 0);
	if (pixels==NULL)
	    return;

	info->lastFrameReaminder = 0;
	if (desiredIdx >= imgCount)
		desiredIdx = imgCount - 1;

	while (info->currentIndex < desiredIdx)
	{
		info->currentIndex++;
        getBitmap((argb *) pixels, info);
	}
	(*env)->ReleaseIntArrayElements(env, jPixels, pixels, 0);
	if (info->speedFactor == 1.0)
		info->nextStartTime = getRealTime()
				+ info->infos[info->currentIndex].duration;
	else
		info->nextStartTime = getRealTime()
                + (unsigned long) (info->infos[info->currentIndex].duration * info->speedFactor);

}

JNIEXPORT jboolean JNICALL
Java_pl_droidsonroids_gif_GifDrawable_renderFrame(JNIEnv * env, jclass class,
		jintArray jPixels, jlong gifInfo, jintArray metaData)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL || jPixels==NULL)
		return JNI_FALSE;
	bool needRedraw = false;
	__time_t rt = getRealTime();
	jboolean isAnimationCompleted;
	if (rt >= info->nextStartTime && info->currentLoop < info->loopCount)
	{
		if (++info->currentIndex >= info->gifFilePtr->ImageCount)
			info->currentIndex = 0;
		needRedraw = true;
		isAnimationCompleted = info->currentIndex >= info->gifFilePtr->ImageCount -1 ?
		    JNI_TRUE : JNI_FALSE;
	}
	else
	    isAnimationCompleted=JNI_FALSE;

	jint* const rawMetaData = (*env)->GetIntArrayElements(env, metaData, 0);
	if (rawMetaData==NULL)
	    return JNI_FALSE;

 	if (needRedraw)
	{
		jint* const pixels = (*env)->GetIntArrayElements(env, jPixels, 0);
		if (pixels==NULL)
		{
		    (*env)->ReleaseIntArrayElements(env, metaData, rawMetaData, 0);
		    return isAnimationCompleted;
		}
        getBitmap((argb *) pixels, info);
		rawMetaData[3] = info->gifFilePtr->Error;

		(*env)->ReleaseIntArrayElements(env, jPixels, pixels, 0);
		unsigned int scaledDuration = info->infos[info->currentIndex].duration;
		if (info->speedFactor != 1.0)
		{
			scaledDuration /= info->speedFactor;
			if (scaledDuration<=0)
			    scaledDuration=1;
			else if (scaledDuration>INT_MAX)
			    scaledDuration=INT_MAX;
		}
		info->nextStartTime = rt + scaledDuration;
		rawMetaData[4] = scaledDuration;
	}
	else
	{
	    long delay=info->nextStartTime-rt;
	    if (delay<0)
	        rawMetaData[4] = -1;
	    else //no need to check upper bound since info->nextStartTime<=rt+INT_MAX always
		    rawMetaData[4] = (int) delay;
	}
	(*env)->ReleaseIntArrayElements(env, metaData, rawMetaData, 0);
	return isAnimationCompleted;
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_free(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	if (gifInfo == (jlong)(intptr_t) NULL)
		return;
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info->rewindFunction == streamRewind)
	{
		StreamContainer* sc = info->gifFilePtr->UserData;
		jmethodID closeMID = (*env)->GetMethodID(env, sc->streamCls, "close",
				"()V");
		if (closeMID != NULL)
			(*env)->CallVoidMethod(env, sc->stream, closeMID);
		if ((*env)->ExceptionOccurred(env))
			(*env)->ExceptionClear(env);

		(*env)->DeleteGlobalRef(env, sc->streamCls);
		(*env)->DeleteGlobalRef(env, sc->stream);

		if (sc->buffer != NULL)
		{
			(*env)->DeleteGlobalRef(env, sc->buffer);
		}

		free(sc);
	}
	else if (info->rewindFunction == fileRewind)
	{
		FILE* file = info->gifFilePtr->UserData;
		fclose(file);
	}
	else if (info->rewindFunction == byteArrayRewind)
	{
		ByteArrayContainer* bac = info->gifFilePtr->UserData;
		if (bac->buffer != NULL)
		{
			(*env)->DeleteGlobalRef(env, bac->buffer);
		}
		free(bac);
	}
	else if (info->rewindFunction == directByteBufferRewindFun)
	{
		DirectByteBufferContainer* dbbc = info->gifFilePtr->UserData;
		free(dbbc);
	}
	info->gifFilePtr->UserData = NULL;
	cleanUp(info);
}

JNIEXPORT jstring JNICALL
Java_pl_droidsonroids_gif_GifDrawable_getComment(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return NULL;
	return (*env)->NewStringUTF(env, info->comment);
}

JNIEXPORT jint JNICALL
Java_pl_droidsonroids_gif_GifDrawable_getLoopCount(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return 0;
	return info->loopCount;
}

JNIEXPORT jint JNICALL
Java_pl_droidsonroids_gif_GifDrawable_getDuration(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return 0;
	int i;
    jint sum = 0;
	for (i = 0; i < info->gifFilePtr->ImageCount; i++)
		sum += info->infos[i].duration;
	return sum;
}

JNIEXPORT jint JNICALL
Java_pl_droidsonroids_gif_GifDrawable_getCurrentPosition(JNIEnv * env,
		jclass class, jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return 0;
	int idx = info->currentIndex;
	if (idx < 0 || info->gifFilePtr->ImageCount <= 1)
		return 0;
	int i;
	unsigned int sum = 0;
	for (i = 0; i < idx; i++)
		sum += info->infos[i].duration;
	__time_t remainder =
			info->lastFrameReaminder == ULONG_MAX ?
					getRealTime() - info->nextStartTime :
					info->lastFrameReaminder;
	return (int) (sum + remainder);
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_saveRemainder(JNIEnv * env, jclass class,
		jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return;
	info->lastFrameReaminder = info->nextStartTime - getRealTime();
}

JNIEXPORT void JNICALL
Java_pl_droidsonroids_gif_GifDrawable_restoreRemainder(JNIEnv * env,
		jclass class, jlong gifInfo)
{
	GifInfo* info =(GifInfo*)(intptr_t) gifInfo;
	if (info == NULL || info->lastFrameReaminder == ULONG_MAX)
		return;
	info->nextStartTime = getRealTime() + info->lastFrameReaminder;
	info->lastFrameReaminder = ULONG_MAX;
}

JNIEXPORT jlong JNICALL
Java_pl_droidsonroids_gif_GifDrawable_getAllocationByteCount(JNIEnv * env,
		jclass class, jlong gifInfo)
{
	GifInfo* info = (GifInfo*)(intptr_t) gifInfo;
	if (info == NULL)
		return 0;
	GifWord pxCount = info->gifFilePtr->SWidth + info->gifFilePtr->SHeight;
	size_t sum = pxCount * sizeof(char);
	if (info->backupPtr != NULL)
		sum += pxCount * sizeof(argb);
	return (jlong) sum;
}

jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
	JNIEnv* env;
	if ((*vm)->GetEnv(vm, (void**) (&env), JNI_VERSION_1_6) != JNI_OK)
	{
		return -1;
	}
	g_jvm = vm;
	defaultCmap = genDefColorMap();
	if (defaultCmap == NULL)
		return -1;
	return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM* vm, void* reserved)
{
	GifFreeMapObject(defaultCmap);
}