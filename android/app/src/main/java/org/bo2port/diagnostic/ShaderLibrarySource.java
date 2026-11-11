// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;


final class ShaderLibrarySource implements ShaderLibraryInstaller.Source {
    private final ContentResolver resolver;
    private final HashMap<String,Uri> files=new HashMap<>();
    ShaderLibrarySource(ContentResolver resolver,Uri tree)throws Exception {
        this.resolver=resolver;
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));
        String[] columns={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME};
        try(Cursor cursor=resolver.query(children,columns,null,null,null)){
            if(cursor==null)throw new IOException("No se pudo leer la carpeta de biblioteca");
            int count=0;
            while(cursor.moveToNext()){
                if(++count>512)throw new IOException("Elige una carpeta que contenga solo los lotes de biblioteca");
                String name=cursor.getString(1);
                if(name==null||!(name.equals(ShaderLibraryInstaller.MANIFEST)||name.matches("bo2-biblioteca-[0-9]{4}\\.zip")))continue;
                if(files.put(name,DocumentsContract.buildDocumentUriUsingTree(tree,cursor.getString(0)))!=null)throw new IOException("Nombre repetido: "+name);
            }
        }
    }
    public InputStream open(String name)throws Exception {
        Uri uri=files.get(name);if(uri==null)throw new IOException("Falta "+name+" en la carpeta elegida");
        InputStream stream=resolver.openInputStream(uri);if(stream==null)throw new IOException("No se pudo abrir "+name);return stream;
    }
}
