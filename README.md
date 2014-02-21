libarsc
=======

pure C++ lib of Android resources.arsc Parser
almost all code transplanted from aapt
cause i use this just in a small executable, mutexes are removed.
this works very well, i promise

example
=======

        resources_content_bin = getResFromApk(apk_path,
                &resources_content_bin_len, ARSC);
        LOGD("resources content len %d", resources_content_bin_len);

        arsc_index = strtoimax(package_label_addr_manifest + 1, NULL, 16);
        if (arsc_index == 0u)
        {
            LOGE("package label index error %s", package_label_addr_manifest);
            break;
        }

        LOGD("package label in manifest %u", arsc_index);

        ResTable res(resources_content_bin, resources_content_bin_len, resources_content_bin);
        std::vector<std::string> * label_vec = res.getLabel(true, arsc_index);

