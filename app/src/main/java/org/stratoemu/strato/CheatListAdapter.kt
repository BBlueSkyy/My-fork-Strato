/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato

import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import org.stratoemu.strato.databinding.ItemContentBinding

class CheatListAdapter(
    private val onToggled: (GameCheat, Boolean) -> Unit,
    private val onDelete: (GameCheat) -> Unit
) : ListAdapter<GameCheat, CheatListAdapter.CheatViewHolder>(DiffCallback()) {

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CheatViewHolder {
        return CheatViewHolder(
            ItemContentBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        )
    }

    override fun onBindViewHolder(holder: CheatViewHolder, position: Int) {
        holder.bind(getItem(position))
    }

    inner class CheatViewHolder(
        private val binding: ItemContentBinding
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(cheat: GameCheat) {
            binding.contentName.text = "${cheat.name}\n${cheat.packName} • ${cheat.buildId}"
            binding.contentTypeIcon.setImageResource(R.drawable.ic_extension)
            binding.radioButton.visibility = View.GONE
            binding.checkBox.visibility = View.VISIBLE
            binding.deleteButton.visibility = View.VISIBLE
            binding.selectionContainer.visibility = View.VISIBLE
            binding.checkBox.isChecked = cheat.enabled
            binding.checkBox.isEnabled = cheat.packEnabled
            binding.root.isEnabled = cheat.packEnabled
            binding.root.alpha = if (cheat.packEnabled) 1f else 0.6f
            binding.checkBox.jumpDrawablesToCurrentState()

            val background = MaterialColors.getColor(
                binding.iconContainer,
                com.google.android.material.R.attr.colorTertiaryContainer
            )
            val foreground = MaterialColors.getColor(
                binding.iconContainer,
                com.google.android.material.R.attr.colorOnTertiaryContainer
            )
            binding.iconContainer.setCardBackgroundColor(background)
            binding.contentTypeIcon.imageTintList = ColorStateList.valueOf(foreground)

            binding.checkBox.setOnClickListener {
                onToggled(cheat, binding.checkBox.isChecked)
            }
            binding.deleteButton.setOnClickListener {
                onDelete(cheat)
            }
            binding.root.setOnClickListener {
                if (!cheat.packEnabled)
                    return@setOnClickListener
                binding.checkBox.isChecked = !binding.checkBox.isChecked
                onToggled(cheat, binding.checkBox.isChecked)
            }
        }
    }

    private class DiffCallback : DiffUtil.ItemCallback<GameCheat>() {
        override fun areItemsTheSame(oldItem: GameCheat, newItem: GameCheat): Boolean {
            return oldItem.file == newItem.file && oldItem.name == newItem.name
        }

        override fun areContentsTheSame(oldItem: GameCheat, newItem: GameCheat): Boolean {
            return oldItem == newItem
        }
    }
}
