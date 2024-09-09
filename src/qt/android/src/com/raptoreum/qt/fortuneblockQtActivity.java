package com.fortuneblock.qt;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;

import org.qtproject.qt5.android.bindings.QtActivity;

import java.io.File;

public class FortuneblockQtActivity extends QtActivity
{
	@Override
	public void onCreate(Bundle savedInstanceState)
	{
		final File fortuneblockDir = new File(getFilesDir().getAbsolutePath() + "/.fortuneblockcore");
		if (!fortuneblockDir.exists()) {
			fortuneblockDir.mkdir();
		}


		super.onCreate(savedInstanceState);
	}
}