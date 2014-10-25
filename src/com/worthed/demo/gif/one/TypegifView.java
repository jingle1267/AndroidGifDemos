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
package com.worthed.demo.gif.one;

import com.worthed.demo.gif.R;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

/**
 * @author jingle1267@163.com
 * 
 */
public class TypegifView extends View implements Runnable {

	gifOpenHelper gHelper;
	private boolean isStop = false;
	int delta = 1;

	Bitmap bmp;

	int count = 0;

	int dH = 0;
	int dW = 0;
	Context mContext;

	// construct - refer for java
	public TypegifView(Context context) {
		this(context, null);
		mContext = context;
	}

	// construct - refer for xml
	public TypegifView(Context context, AttributeSet attrs) {
		super(context, attrs);
		mContext = context;
		init();
		TypedArray ta = context.obtainStyledAttributes(attrs,
				R.styleable.gifView);
		int n = ta.getIndexCount();

		for (int i = 0; i < n; i++) {
			int attr = ta.getIndex(i);

			switch (attr) {
			case R.styleable.gifView_src:
				int id = ta.getResourceId(R.styleable.gifView_src, 0);
				setSrc(id);
				break;

			case R.styleable.gifView_delay:
				int idelta = ta.getInteger(R.styleable.gifView_delay, 1);
				setDelta(idelta);
				break;

			case R.styleable.gifView_stop:
				boolean sp = ta.getBoolean(R.styleable.gifView_stop, false);
				if (!sp) {
					setStop();
				}
				break;
			}

		}

		ta.recycle();

	}

	private void init() {
		dW = mContext.getResources().getDisplayMetrics().widthPixels;
		dH = mContext.getResources().getDisplayMetrics().heightPixels;
	}

	/**
     *
     */
	public void setStop() {
		isStop = true;
	}

	/**
     *
     */
	public void setStart() {

		count = 0;
		isStop = false;

		Thread updateTimer = new Thread(this);
		updateTimer.start();

	}

	/**
     *
     */
	public void setSrc(int id) {

		mWidth = dW;
		mHeight = dH;

		gHelper = new gifOpenHelper();
		gHelper.read(TypegifView.this.getResources().openRawResource(id));
		bmp = gHelper.getImage();
		bmp = Bitmap.createScaledBitmap(bmp, mWidth, mHeight, false);
	}

	public void release() {
		for (int i = 0; i < gHelper.getFrameCount(); i++) {
			Bitmap b = gHelper.nextBitmap();
			if (b != null && !b.isRecycled())
				b.recycle();
		}
	}

	public void setDelta(int is) {
		delta = is;
	}

	// to meaure its Width & Height
	int mHeight = 0;
	int mWidth = 0;

	@Override
	protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
		super.onMeasure(widthMeasureSpec, heightMeasureSpec);
	}

	private int measureWidth(int measureSpec) {
		return gHelper.getWidth();
	}

	private int measureHeight(int measureSpec) {
		return gHelper.getHeigh();
	}

	protected void onDraw(Canvas canvas) {

		canvas.drawBitmap(bmp, 0, 0, new Paint());
		if (count != 0) {
			bmp.recycle();
		}
		bmp = gHelper.nextBitmap();
		bmp = Bitmap.createScaledBitmap(bmp, mWidth, mHeight, false);
		count++;
		if (count == (gHelper.getFrameCount() - 1)) {
			isStop = true;
		}
	}

	public void run() {

		while (!isStop) {
			try {
				this.postInvalidate();
				Thread.sleep(gHelper.nextDelay() / delta);
			} catch (Exception ex) {
				ex.printStackTrace();
			}
		}
	}

}
