/**
 * Copyright 2014 Zhenguo Jin (jinzhenguo1990@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.worthed.demo.gif.two;

import com.worthed.demo.gif.R;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Movie;
import android.util.AttributeSet;
import android.view.View;

/**
 * @author jingle1267@163.com
 *
 */
public class MyGifView extends View {

	private long movieStart;
	private Movie movie;
	    //此处必须重写该构造方法
	public MyGifView(Context context,AttributeSet attributeSet) {
	super(context,attributeSet);
	//以文件流（InputStream）读取进gif图片资源
	movie=Movie.decodeStream(getResources().openRawResource(R.drawable.intro_index));
	}
	 
	@Override
	protected void onDraw(Canvas canvas) {
	long curTime=android.os.SystemClock.uptimeMillis();
	//第一次播放
	if (movieStart == 0) {
	movieStart = curTime;
	}
	if (movie != null) {
	int duraction = movie.duration();
	int relTime = (int) ((curTime-movieStart)%duraction);
	movie.setTime(relTime);
	movie.draw(canvas, 0, 0);
	//强制重绘
	invalidate();
	}
	super.onDraw(canvas);
	}
	
}
